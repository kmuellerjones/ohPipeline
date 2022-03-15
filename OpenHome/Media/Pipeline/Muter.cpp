#include <OpenHome/Media/Pipeline/Muter.h>
#include <OpenHome/Types.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Functor.h>

#include <algorithm>

using namespace OpenHome;
using namespace OpenHome::Media;

const TUint Muter::kSupportedMsgTypes =   eMode
                                        | eTrack
                                        | eDrain
                                        | eEncodedStream
                                        | eMetatext
                                        | eStreamInterrupted
                                        | eHalt
                                        | eDecodedStream
                                        | eAudioPcm
                                        | eAudioDsd
                                        | eSilence
                                        | eQuit;

Muter::Muter(MsgFactory& aMsgFactory, IPipelineElementUpstream& aUpstream, TUint aRampDuration)
    : PipelineElement(kSupportedMsgTypes)
    , iMsgFactory(aMsgFactory)
    , iUpstream(aUpstream)
    , iAnimator(nullptr)
    , iLock("MPMT")
    , iSemMuted("MPMT", 0)
    , iState(eRunning)
    , iRampDuration(aRampDuration)
    , iRemainingRampSize(0)
    , iCurrentRampValue(Ramp::kMax)
    , iMsgHalt(nullptr)
    , iMsgDrain(nullptr)
    , iHalting(false)
    , iHalted(true)
{
}

Muter::~Muter()
{
    if (iMsgHalt != nullptr) {
        iMsgHalt->RemoveRef();
    }
    if (iMsgDrain != nullptr) {
        iMsgDrain->RemoveRef();
    }
}

void Muter::SetAnimator(IPipelineAnimator& aPipelineAnimator)
{
    iAnimator = &aPipelineAnimator;
}

void Muter::Mute()
{
    LOG(kPipeline, "Muter::Mute\n");
    TBool block = false;
    {
        AutoMutex _(iLock);
        if (iState == eRunning) {
            if (iHalted) {
                iState = eMuted;
            }
            else if (iHalting) {
                iState = eMuting;
                block = true;
            }
            else {
                iState = eRampingDown;
                iRemainingRampSize = iRampDuration;
                iCurrentRampValue = Ramp::kMax;
                block = true;
            }
        }
        else if (iState == eRampingUp) {
            if (iRemainingRampSize == iRampDuration) {
                iState = eMuted;
            }
            else {
                iState = eRampingDown;
                iRemainingRampSize = iRampDuration - iRemainingRampSize;
                block = true;
            }
        }
        else { // shouldn't be possible to be called for remaining states
            ASSERTS();
        }
        if (block) {
            (void)iSemMuted.Clear();
        }
    }
    if (block) {
        iSemMuted.Wait();
    }
}

void Muter::Unmute()
{
    LOG(kPipeline, "Muter::Unmute\n");
    AutoMutex _(iLock);
    switch (iState)
    {
    case eRunning:
    case eRampingUp:
        // not supported - error in upstream IMute?
        ASSERTS();
        break;
    case eRampingDown:
        iSemMuted.Signal();
        if (iRemainingRampSize == iRampDuration) {
            iState = eRunning;
        }
        else {
            iState = eRampingUp;
            iRemainingRampSize = iRampDuration - iRemainingRampSize;
        }
        break;
    case eMuting:
        iSemMuted.Signal();
        iHalting = false;
        iState = eRampingUp;
        iRemainingRampSize = iRampDuration;
        iCurrentRampValue = Ramp::kMin;
        break;
    case eMuted:
        if (iHalted) {
            iState = eRunning;
        }
        else {
            iState = eRampingUp;
            iRemainingRampSize = iRampDuration;
            iCurrentRampValue = Ramp::kMin;
        }
        break;
    }
}

Msg* Muter::Pull()
{
    Msg* msg;
    if (!iQueue.IsEmpty()) {
        msg = iQueue.Dequeue();
    }
    else {
        msg = iUpstream.Pull();
    }
    iLock.Wait();
    msg = msg->Process(*this);
    iLock.Signal();
    ASSERT(msg != nullptr);
    return msg;
}

Msg* Muter::ProcessMsg(MsgDrain* aMsg)
{
    ASSERT(iMsgDrain == nullptr);
    iMsgDrain = aMsg;
    BeginHalting();
    return iMsgFactory.CreateMsgDrain(MakeFunctor(*this, &Muter::PipelineDrained));
}

Msg* Muter::ProcessMsg(MsgHalt* aMsg)
{
    if (iMsgHalt != nullptr) {
        return aMsg;
    }
    iMsgHalt = aMsg;
    BeginHalting();
    return iMsgFactory.CreateMsgHalt(aMsg->Id(), MakeFunctor(*this, &Muter::PipelineHalted));
}

Msg* Muter::ProcessMsg(MsgAudioPcm* aMsg)
{
    return ProcessAudio(aMsg);
}

Msg* Muter::ProcessMsg(MsgAudioDsd* aMsg)
{
    return ProcessAudio(aMsg);
}

Msg* Muter::ProcessMsg(MsgSilence* aMsg)
{
    switch (iState)
    {
    case eRunning:
    case eMuting:
    case eMuted:
        break;
    case eRampingDown:
        iState = eMuting;
        iRemainingRampSize = 0;
        iCurrentRampValue = Ramp::kMin;
        break;
    case eRampingUp:
        iState = eRunning;
        iRemainingRampSize = 0;
        iCurrentRampValue = Ramp::kMax;
        break;
    }
    return aMsg;
}

Msg* Muter::ProcessAudio(MsgAudioDecoded* aMsg)
{
    iHalting = iHalted = false;
    MsgAudio* msg = aMsg;
    switch (iState)
    {
    case eRunning:
        break;
    case eRampingDown:
    case eRampingUp:
    {
        MsgAudio* split;
        if (msg->Jiffies() > iRemainingRampSize && iRemainingRampSize > 0) {
            split = msg->Split(iRemainingRampSize);
            if (split != nullptr) {
                iQueue.EnqueueAtHead(split);
            }
        }
        split = nullptr;
        const Ramp::EDirection direction = (iState == eRampingDown? Ramp::EDown : Ramp::EUp);
        if (iRemainingRampSize > 0) {
            iCurrentRampValue = msg->SetRamp(iCurrentRampValue, iRemainingRampSize, direction, split);
        }
        if (iRemainingRampSize == 0) {
            if (iState == eRampingUp) {
                iState = eRunning;
            }
            else {
                iState = eMuting;
                iJiffiesUntilMute = iAnimator->PipelineAnimatorBufferJiffies();
            }
        }
        if (split != nullptr) {
            iQueue.EnqueueAtHead(split);
        }
    }
        break;
    case eMuting:
        if (iJiffiesUntilMute == 0) {
            iState = eMuted;
            iSemMuted.Signal();
        }
        else {
            iJiffiesUntilMute -= std::min(aMsg->Jiffies(), iJiffiesUntilMute);
        }
        // fallthrough
    case eMuted:
        aMsg->SetMuted();
        break;
    }

    return msg;
}

void Muter::BeginHalting()
{
    iHalting = true;
    if (iState == eRampingDown) {
        iState = eMuting;
        iRemainingRampSize = 0;
        iCurrentRampValue = Ramp::kMin;
    }
}

void Muter::Halted()
{
    if (iHalting) {
        iHalted = true;
    }
    iJiffiesUntilMute = 0;
    iSemMuted.Signal();
    if (iState == eMuting) {
        iState = eMuted;
    }
}

void Muter::PipelineHalted()
{
    AutoMutex _(iLock);
    Halted();
    if (iMsgHalt != nullptr) {
        iMsgHalt->ReportHalted();
        iMsgHalt->RemoveRef();
        iMsgHalt = nullptr;
    }
}

void Muter::PipelineDrained()
{
    AutoMutex _(iLock);
    if (iHalting) {
        iHalted = true;
    }
    iJiffiesUntilMute = 0;
    iSemMuted.Signal();
    if (iState == eMuting) {
        iState = eMuted;
    }
}
