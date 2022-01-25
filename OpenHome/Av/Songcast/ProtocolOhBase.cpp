#include <OpenHome/Av/Songcast/ProtocolOhBase.h>
#include <OpenHome/Types.h>
#include <OpenHome/Media/Protocol/Protocol.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Av/Songcast/OhmMsg.h>
#include <OpenHome/Av/Songcast/Ohm.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Av/Debug.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Private/Timer.h>
#include <OpenHome/Private/Uri.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Private/Stream.h>
#include <OpenHome/Private/Env.h>
#include <OpenHome/OsWrapper.h>
#include <OpenHome/Private/NetworkAdapterList.h>
#include <OpenHome/Private/TIpAddressUtils.h>

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Media;

ProtocolOhBase::ProtocolOhBase(Environment& aEnv, IOhmMsgFactory& aFactory, Media::TrackFactory& aTrackFactory,
                               Optional<IOhmTimestamper> aTimestamper, const TChar* aSupportedScheme, const Brx& aMode,
                               Optional<Av::IOhmMsgProcessor> aOhmMsgProcessor)
    : Protocol(aEnv)
    , iEnv(aEnv)
    , iMsgFactory(aFactory)
    , iSupply(nullptr)
    , iSocket(aEnv)
    , iReadBuffer(iSocket)
    , iMode(aMode)
    , iStreamId(IPipelineIdProvider::kStreamIdInvalid)
    , iMutexTransport("POHB")
    , iTimestamper(aTimestamper.Ptr())
    , iStarving(false)
    , iTrackFactory(aTrackFactory)
    , iSupportedScheme(aSupportedScheme)
    , iRunning(false)
    , iRepairing(false)
    , iTrackMsgDue(false)
    , iStreamMsgDue(true)
    , iMetatextMsgDue(false)
    , iSeqTrackValid(false)
    , iSeqTrack(UINT_MAX)
    , iLastSampleStart(UINT_MAX)
    , iBitDepth(0)
    , iSampleRate(0)
    , iNumChannels(0)
    , iLatency(0)
    , iRepairFirst(nullptr)
    , iPipelineEmpty("OHBS", 0)
    , iOhmMsgProcessor(aOhmMsgProcessor)
{
    iNacnId = iEnv.NetworkAdapterList().AddCurrentChangeListener(MakeFunctor(*this, &ProtocolOhBase::CurrentSubnetChanged), "ProtocolOhBase", false);
    iTimerRepair = new Timer(aEnv, MakeFunctor(*this, &ProtocolOhBase::TimerRepairExpired), "ProtocolOhBaseRepair");
    iRepairFrames.reserve(kMaxRepairBacklogFrames);
    iTimerJoin = new Timer(aEnv, MakeFunctor(*this, &ProtocolOhBase::SendJoin), "ProtocolOhBaseJoin");
    iTimerListen = new Timer(aEnv, MakeFunctor(*this, &ProtocolOhBase::SendListen), "ProtocolOhBaseListen");

    AutoNetworkAdapterRef ref(iEnv, "Songcast");
    const auto current = ref.Adapter();
    iAddr = (current == nullptr? kIpAddressV4AllAdapters : current->Address());
}

ProtocolOhBase::~ProtocolOhBase()
{
    iEnv.NetworkAdapterList().RemoveCurrentChangeListener(iNacnId);
    delete iTimerRepair;
    delete iTimerJoin;
    delete iTimerListen;
    delete iSupply;
}

void ProtocolOhBase::Add(OhmMsg* aMsg)
{
    aMsg->Process(*this);
    if (iOhmMsgProcessor.Ok()) {
        aMsg->Process(iOhmMsgProcessor.Unwrap());
    }
}

void ProtocolOhBase::ResendSeen()
{
    iMutexTransport.Wait();
    if (iRepairing) {
        iTimerRepair->FireIn(kSubsequentRepairTimeoutMs);
    }
    iMutexTransport.Signal();
}

void ProtocolOhBase::RequestResend(const Brx& aFrames)
{
    const TUint bytes = aFrames.Bytes();
    if (bytes > 0) {
        Bws<OhmHeader::kHeaderBytes + 400> buffer;
        WriterBuffer writer(buffer);
        OhmHeaderResend headerResend(bytes / 4);
        OhmHeader header(OhmHeader::kMsgTypeResend, headerResend.MsgBytes());
        header.Externalise(writer);
        headerResend.Externalise(writer);
        writer.Write(aFrames);
        try {
            iSocket.Send(buffer, iEndpoint);
        }
        catch (NetworkError&) {
            LOG_ERROR(kSongcast, "NetworkError in ProtocolOhBase::RequestResend()\n");
        }
    }
}

void ProtocolOhBase::SendJoin()
{
    LOG(kSongcast, "SendJoin\n");
    Send(OhmHeader::kMsgTypeJoin);
    iTimerJoin->FireIn(kTimerJoinTimeoutMs);
}

void ProtocolOhBase::SendListen()
{
    Send(OhmHeader::kMsgTypeListen);
    iTimerListen->FireIn((kTimerListenTimeoutMs >> 2) - iEnv.Random(kTimerListenTimeoutMs >> 3)); // listen primary timeout
}

void ProtocolOhBase::Send(TUint aType)
{
    Bws<OhmHeader::kHeaderBytes> buffer;
    WriterBuffer writer(buffer);
    OhmHeader msg(aType, 0);
    msg.Externalise(writer);
    try {
        iSocket.Send(buffer, iEndpoint);
    }
    catch (NetworkError&) {
    }
}

TBool ProtocolOhBase::IsCurrentStream(TUint aStreamId) const
{
    if (iStreamId != aStreamId || aStreamId == IPipelineIdProvider::kStreamIdInvalid) {
        return false;
    }
    return true;
}

void ProtocolOhBase::WaitForPipelineToEmpty()
{
    LOG(kSongcast, "> ProtocolOhBase::WaitForPipelineToEmpty()\n");
    iSupply->OutputDrain(MakeFunctor(iPipelineEmpty, &Semaphore::Signal));
    try {
        iPipelineEmpty.Wait(ISupply::kMaxDrainMs);
    }
    catch (Timeout&) {
        LOG(kPipeline, "WARNING: ProtocolOhBase: timeout draining pipeline\n");
        ASSERTS();
    }
    {
        AutoMutex _(iMutexTransport);
        RepairReset(); // allow for clean restart of stream following a drain
    }
    LOG(kSongcast, "< ProtocolOhBase::WaitForPipelineToEmpty()\n");
}

void ProtocolOhBase::Interrupt(TBool aInterrupt)
{
    iSocket.Interrupt(aInterrupt);
}

void ProtocolOhBase::Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream)
{
    iSupply = new Supply(aMsgFactory, aDownstream);
}

ProtocolStreamResult ProtocolOhBase::Stream(const Brx& aUri)
{
    iUri.Replace(aUri);
    if (iUri.Scheme() != iSupportedScheme) {
        return EProtocolErrorNotSupported;
    }
    iStarving = false;
    iSocket.Interrupt(false);
    Endpoint ep;
    try {
        ep.SetPort(iUri.Port());
        ep.SetAddress(iUri.Host());
    }
    catch (NetworkError&) {
        return EProtocolStreamErrorUnrecoverable;
    }
    ProtocolStreamResult res;
    do {
        iMutexTransport.Wait();
        TIpAddress addr = iAddr;
        iMutexTransport.Signal();
        if (TIpAddressUtils::IsZero(addr)) {
            // no current subnet so no hope of listening to another device
            return EProtocolStreamErrorUnrecoverable;
        }
        res = Play(addr, kTtl, ep);
    } while (res != EProtocolStreamStopped);

    iMutexTransport.Wait();
    RepairReset();
    iFrame = 0;
    iTrackMsgDue = true;
    iStreamMsgDue = true;
    iMetatextMsgDue = false;
    iSeqTrackValid = false;
    iMetatext.Replace(Brx::Empty());
    iSupply->OutputMetadata(Brx::Empty());
    iSeqTrack = UINT_MAX;
    iLastSampleStart = UINT_MAX;
    iBitDepth = iSampleRate = iNumChannels = 0;
    iLatency = 0;
    iStreamId = IPipelineIdProvider::kStreamIdInvalid;
    iTrackUri.Replace(Brx::Empty());
    iTrackMetadata.Replace(Brx::Empty());
    iMutexTransport.Signal();

    return res;
}

ProtocolGetResult ProtocolOhBase::Get(IWriter& /*aWriter*/, const Brx& /*aUri*/, TUint64 /*aOffset*/, TUint /*aBytes*/)
{
    return EProtocolGetErrorNotSupported;
}

EStreamPlay ProtocolOhBase::OkToPlay(TUint aStreamId)
{
    auto canPlay = iIdProvider->OkToPlay(aStreamId);
    ASSERT(canPlay != ePlayLater);
    return canPlay;
}

void ProtocolOhBase::NotifyStarving(const Brx& aMode, TUint aStreamId, TBool aStarving)
{
    if (aMode == iMode && aStarving) {
        LOG(kSongcast, "ProtocolOhBase::NotifyStarving for stream %u\n", aStreamId);
        iStarving = true;
        iSocket.Interrupt(true);
    }
}

void ProtocolOhBase::CurrentSubnetChanged()
{
    AutoNetworkAdapterRef ref(iEnv, "ProtocolOhBase");
    iMutexTransport.Wait();
    const auto current = ref.Adapter();
    iAddr = (current == nullptr? kIpAddressV4AllAdapters : current->Address());
    iMutexTransport.Signal();
    iSocket.ReadInterrupt();
}

TBool ProtocolOhBase::RepairBegin(OhmMsgAudio& aMsg)
{
    LOG(kSongcast, "BEGIN ON %d\n", aMsg.Frame());
    iRepairFirst = &aMsg;
    iTimerRepair->FireIn(iEnv.Random(kInitialRepairTimeoutMs)); 
    return true;
}

void ProtocolOhBase::RepairReset()
{
    LOG(kSongcast, "RESET\n");
    /* TimerRepairExpired() claims iMutexTransport.  Release it briefly to avoid possible deadlock.
       TimerManager guarantees that TimerRepairExpired() won't be called once Cancel() returns... */
    iMutexTransport.Signal();
    iTimerRepair->Cancel();
    iMutexTransport.Wait();
    if (iRepairFirst != nullptr) {
        iRepairFirst->RemoveRef();
        iRepairFirst = nullptr;
    }
    for (TUint i=0; i<iRepairFrames.size(); i++) {
        iRepairFrames[i]->RemoveRef();
    }
    iRepairFrames.clear();
    iRunning = false;
    iRepairing = false; // FIXME - not absolutely required as test for iRunning takes precedence in Process(OhmMsgAudio&
    iStreamMsgDue = true; // a failed repair implies a discontinuity in audio.  This should be noted as a new stream.
}

TBool ProtocolOhBase::Repair(OhmMsgAudio& aMsg)
{
    // get the incoming frame number
    const TUint frame = aMsg.Frame();
    LOG(kSongcast, "GOT %d\n", frame);

    // get difference between this and the last frame sent down the pipeline
    TInt diff = frame - iFrame;
    if (diff < 1) {
        TBool repairing = true;
        if (!aMsg.Resent()) {
            // A frame in the past that is not a resend implies that the sender has reset their frame count
            RepairReset();
            repairing = false;
        }
        // incoming frames is equal to or earlier than the last frame sent down the pipeline
        // in other words, it's a duplicate, so discard it and continue
        aMsg.RemoveRef();
        return repairing;
    }
    if (diff > (TInt)kMaxRepairBacklogFrames) {
        // we're so far behind that we can't fit all the missing frames into iRepairFrames
        RepairReset();
        aMsg.RemoveRef();
        return false;
    }
    if (diff == 1) {
        // incoming frame is one greater than the last frame sent down the pipeline, so send this ...
        iFrame++;
        OutputAudio(aMsg);
        // ... and see if the current first waiting frame is now also ready to be sent
        while (iRepairFirst->Frame() == iFrame + 1) {
            // ... yes, it is, so send it
            iFrame++;
            OutputAudio(*iRepairFirst);
            // ... and see if there are further messages waiting
            if (iRepairFrames.size() == 0) {
                // ... no, so we have completed the repair
                iRepairFirst = nullptr;
                LOG(kSongcast, "END\n");
                return false;
            }
            // ... yes, so update the current first waiting frame and continue testing to see if this can also be sent
            iRepairFirst = iRepairFrames[0];
            iRepairFrames.erase(iRepairFrames.begin());
        }
        // ... we're done
        return true;
    }

    // Ok, its a frame that needs to be put into the backlog, but where?
    // compare it to the current first waiting frame
    diff = frame - iRepairFirst->Frame();
    if (diff == 0) {
        // it's equal to the currently first waiting frame, so discard it - it's a duplicate
        aMsg.RemoveRef();
        return true;
    }
    if (diff < 0) {
        // it's earlier than the current first waiting message, so it should become the new current first waiting frame
        // and the old first waiting frame needs to be injected into the start of the backlog, so inject it into the end
        // and rotate the others (if there is space to add another frame)
        if (iRepairFrames.size() == kMaxRepairBacklogFrames) {
            // can't fit another frame into the backlog
            RepairReset();
            aMsg.RemoveRef();
            return false;
        }
        iRepairFrames.insert(iRepairFrames.begin(), iRepairFirst);
        iRepairFirst = &aMsg;
        return true;
    }
    // ok, it's after the currently first waiting frame, so it needs to go into the backlog
    // first check if the backlog is empty
    if (iRepairFrames.size() == 0) {
        // ... yes, so just inject it
        iRepairFrames.insert(iRepairFrames.begin(), &aMsg);
        return true;
    }
    // ok, so the backlog is not empty
    // is it a duplicate of the last frame in the backlog?
    diff = frame - iRepairFrames[iRepairFrames.size()-1]->Frame();
    if (diff == 0) {
        // ... yes, so discard
        aMsg.RemoveRef();
        return true;
    }
    // is the incoming frame later than the last one currently in the backlog?
    if (diff > 0) {
        // ... yes, so, again, just inject it (if there is space)
        if (iRepairFrames.size() == kMaxRepairBacklogFrames) {
            // can't fit another frame into the backlog
            RepairReset();
            aMsg.RemoveRef();
            return false;
        }
        iRepairFrames.push_back(&aMsg);
        return true;
    }
    // ... no, so it has to go somewhere in the middle of the backlog, so iterate through and inject it at the right place (if there is space)
    TUint count = iRepairFrames.size();
    for (auto it = iRepairFrames.begin(); it != iRepairFrames.end(); ++it) {
        diff = frame - (*it)->Frame();
        if (diff > 0) {
            continue;
        }
        if (diff == 0) {
            aMsg.RemoveRef();
        }
        else {
            if (count == kMaxRepairBacklogFrames) {
                // can't fit another frame into the backlog
                aMsg.RemoveRef();
                RepairReset();
                return false;
            }
            iRepairFrames.insert(it, &aMsg);
        }
        break;
    }

    return true;
}

void ProtocolOhBase::TimerRepairExpired()
{
    AutoMutex a(iMutexTransport);
    if (iRepairing) {
        LOG(kSongcast, "REQUEST RESEND");
        Bws<kMaxRepairMissedFrames * 4> missed;
        WriterBuffer buffer(missed);
        WriterBinary writer(buffer);

        TUint count = 0;
        TUint start = iFrame + 1;
        TUint end = iRepairFirst->Frame();

        // phase 1 - request the frames between the last sent down the pipeline and the first waiting frame
        for (TUint i = start; i < end; i++) {
            writer.WriteUint32Be(i);
            LOG(kSongcast, " %d", i);
            if (++count == kMaxRepairMissedFrames) {
                break;
            }
        }

        // phase 2 - if there is room add the missing frames in the backlog
        for (TUint j = 0; count < kMaxRepairMissedFrames && j < iRepairFrames.size(); j++) {
            OhmMsgAudio* msg = iRepairFrames[j];
            start = end + 1;
            end = msg->Frame();
            for (TUint i = start; i < end; i++) {
                writer.WriteUint32Be(i);
                LOG(kSongcast, " %d", i);
                if (++count == kMaxRepairMissedFrames) {
                    break;
                }
            }
        }
        LOG(kSongcast, "\n");

        RequestResend(missed);
        iTimerRepair->FireIn(kSubsequentRepairTimeoutMs);
    }
}

void ProtocolOhBase::AddRxTimestamp(OhmMsgAudio& aMsg)
{
    if (iTimestamper != nullptr) {
        try {
            aMsg.SetRxTimestamp(iTimestamper->Timestamp(aMsg.Frame()));
        }
        catch (OhmTimestampNotFound&) {
            //LOG(kSongcast, "OHM - OhmTimestampNotFound for frame #%u\n", aMsg.Frame());
        }
    }
}

void ProtocolOhBase::OutputAudio(OhmMsgAudio& aMsg)
{
    TBool startOfStream = false;
    if (aMsg.SampleStart() < iLastSampleStart || iBitDepth != aMsg.BitDepth() ||
        iSampleRate != aMsg.SampleRate() || iNumChannels != aMsg.Channels()) {
        startOfStream = true;
        iStreamMsgDue = true;

    }
    if (startOfStream || iTrackMsgDue) {
        Track* track = iTrackFactory.CreateTrack(iTrackUri, iTrackMetadata);
        iSupply->OutputTrack(*track, startOfStream);
        track->RemoveRef();
        iTrackMsgDue = false;
    }
    iLastSampleStart = aMsg.SampleStart();
    if (iStreamMsgDue) {
        const TUint64 totalBytes = static_cast<TUint64>(aMsg.SamplesTotal()) * aMsg.Channels() * aMsg.BitDepth()/8;
        iStreamId = iIdProvider->NextStreamId();
        PcmStreamInfo pcmStream;
        pcmStream.Set(aMsg.BitDepth(), aMsg.SampleRate(), aMsg.Channels(), AudioDataEndian::Big, SpeakerProfile((aMsg.Channels() == 1) ? 1 : 2), aMsg.SampleStart());
        pcmStream.SetCodec(aMsg.Codec(), true);
        iSupply->OutputPcmStream(iTrackUri, totalBytes, false/*seekable*/, false/*live*/, Multiroom::Forbidden, *this, iStreamId, pcmStream);
        iStreamMsgDue = false;
        iBitDepth = aMsg.BitDepth();
        // iSampleRate updated below
        iNumChannels = aMsg.Channels();
    }
    if (iSampleRate != aMsg.SampleRate() || iLatency != aMsg.MediaLatency()) {
        iSampleRate = aMsg.SampleRate();
        iLatency = aMsg.MediaLatency();
        const TUint delayJiffies = static_cast<TUint>(Jiffies::FromSongcastTime(iLatency, iSampleRate));
        iSupply->OutputDelay(delayJiffies);
        if (iTimestamper != nullptr) {
            iTimestamper->SetSampleRate(iSampleRate);
        }
    }
    if (iMetatextMsgDue) {
        iSupply->OutputMetadata(iPendingMetatext);
        iPendingMetatext.Replace(Brx::Empty());
        iMetatextMsgDue = false;
    }
    iSupply->OutputData(aMsg.Audio());
    const TBool halt = aMsg.Halt();
    if (halt) {
        iSupply->OutputWait();
        iSupply->OutputHalt();
    }
    aMsg.RemoveRef();
    if (halt) {
        THROW(OhmDiscontinuity);
    }
}

void ProtocolOhBase::Process(OhmMsgAudio& aMsg)
{
    AddRxTimestamp(aMsg);

    TBool outputAudio = false;
    {
        AutoMutex _(iMutexTransport);
        if (!iRunning) {
            iFrame = aMsg.Frame();
            iRunning = true;
            outputAudio = true;
        }
        else if (iRepairing) {
            iRepairing = Repair(aMsg);
        }
        else {
            const TInt diff = aMsg.Frame() - iFrame;
            if (diff == 1) {
                iFrame++;
                outputAudio = true;
            }
            else if (diff < 1) {
                const TBool resent = aMsg.Resent();
                aMsg.RemoveRef();
                if (!resent) {
                    // A frame in the past that is not a resend implies that the sender has reset their frame count
                    // force recently output audio to ramp down
                    THROW(ReaderError);
                }
            }
            else {
                iRepairing = RepairBegin(aMsg);
            }
        }
    }
    if (outputAudio) {
        OutputAudio(aMsg);
    }
}

void ProtocolOhBase::Process(OhmMsgTrack& aMsg)
{
    if (!iSeqTrackValid || iSeqTrack != aMsg.Sequence()) {
        iSeqTrackValid = true;
        iSeqTrack = aMsg.Sequence();
        iTrackUri.Replace(aMsg.Uri());
        iTrackMetadata.Replace(aMsg.Metadata());
        iTrackMsgDue = true;
    }
    aMsg.RemoveRef();
}

void ProtocolOhBase::Process(OhmMsgMetatext& aMsg)
{
    if (iMetatext != aMsg.Metatext()) {
        iMetatext.Replace(aMsg.Metatext());
        if (iTrackMsgDue) {
            // Pipeline expects a stream before any metatext.  Buffer metatext until we can output a stream.
            iMutexTransport.Wait();
            iMetatextMsgDue = true;
            iPendingMetatext.Replace(aMsg.Metatext());
            iMutexTransport.Signal();
        }
        else {
            iSupply->OutputMetadata(aMsg.Metatext());
        }
    }
    aMsg.RemoveRef();
}
