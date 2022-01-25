#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Media/Pipeline/Msg.h>

namespace OpenHome {
namespace Media {

/*
Left-most pipeline element.
Creates pipeline messages based on requests from protocol modules or Pipeline
*/

class Supply : public ISupply, private INonCopyable
{
public:
    Supply(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownStreamElement);
    virtual ~Supply();
public: // from ISupply
    void OutputTrack(Track& aTrack, TBool aStartOfStream = true) override;
    void OutputDrain(Functor aCallback) override;
    void OutputDelay(TUint aJiffies) override;
    void OutputStream(const Brx& aUri, TUint64 aTotalBytes, TUint64 aStartPos, TBool aSeekable, TBool aLive, Media::Multiroom aMultiroom, IStreamHandler& aStreamHandler, TUint aStreamId, TUint aSeekPosMs = 0) override;
    void OutputPcmStream(const Brx& aUri, TUint64 aTotalBytes, TBool aSeekable, TBool aLive, Media::Multiroom aMultiroom, IStreamHandler& aStreamHandler, TUint aStreamId, const PcmStreamInfo& aPcmStream) override;
    void OutputPcmStream(const Brx& aUri, TUint64 aTotalBytes, TBool aSeekable, TBool aLive, Media::Multiroom aMultiroom, IStreamHandler& aStreamHandler, TUint aStreamId, const PcmStreamInfo& aPcmStream, RampType aRamp) override;
    void OutputDsdStream(const Brx& aUri, TUint64 aTotalBytes, TBool aSeekable, IStreamHandler& aStreamHandler, TUint aStreamId, const DsdStreamInfo& aDsdStream) override;
    void OutputSegment(const Brx& aId) override;
    void OutputData(const Brx& aData) override;
    void OutputMetadata(const Brx& aMetadata) override;
    void OutputHalt(TUint aHaltId = MsgHalt::kIdNone) override;
    void OutputFlush(TUint aFlushId) override;
    void OutputWait() override;
private:
    MsgFactory& iMsgFactory;
    IPipelineElementDownstream& iDownStreamElement;
};

} // namespace Media
} // namespace OpenHome
