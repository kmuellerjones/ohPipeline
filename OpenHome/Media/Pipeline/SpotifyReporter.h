#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Private/Fifo.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Pipeline/AudioReservoir.h>
#include <OpenHome/Media/Pipeline/Reporter.h>

namespace OpenHome {
    class IThreadPool;
    class IThreadPoolHandle;
    enum class ThreadPoolPriority;
namespace Media {

class ISpotifyPlaybackObserver
{
public:
    virtual ~ISpotifyPlaybackObserver() {}
    virtual void NotifyTrackLength(TUint aStreamId, TUint aLengthMs) = 0;
    virtual void NotifyTrackError(TUint aStreamId, TUint aErrorPosMs, const Brx& aReason) = 0;
    virtual void NotifyPlaybackStarted(TUint aStreamId) = 0;
    virtual void NotifyPlaybackContinued(TUint aStreamId) = 0;
    virtual void NotifyPlaybackFinishedNaturally(TUint aStreamId, TUint aLastPosMs) = 0;
};

class ISpotifyReporter
{
public:
    static const TUint kStreamIdInvalid = 0;
public:
    virtual ~ISpotifyReporter() {}
    virtual void AddSpotifyPlaybackObserver(ISpotifyPlaybackObserver& aObserver) = 0;
    virtual TUint64 SubSamples() const = 0;
    virtual void GetPlaybackPosMs(TUint& aStreamId, TUint& aPos) = 0; // Get stream ID and playback pos in an atomic manner.
    virtual void Flush(TUint aFlushId) = 0; // Do not increment subsample count until aFlushId passes.
};

class ISpotifyMetadata
{
public:
    virtual const Brx& PlaybackSource() const = 0;
    virtual const Brx& PlaybackSourceUri() const = 0;
    virtual const Brx& Track() const = 0;
    virtual const Brx& TrackUri() const = 0;
    virtual const Brx& Artist() const = 0;
    virtual const Brx& ArtistUri() const = 0;
    virtual const Brx& Album() const = 0;
    virtual const Brx& AlbumUri() const = 0;
    virtual const Brx& AlbumCoverUri() const = 0;
    virtual const Brx& AlbumCoverUrl() const = 0;
    virtual TUint DurationMs() const = 0;
    virtual TUint Bitrate() const = 0;
    virtual ~ISpotifyMetadata() {}
};

class ISpotifyMetadataAllocated
{
public:
    virtual const ISpotifyMetadata& Metadata() const = 0;
    virtual void AddReference() = 0;
    virtual void RemoveReference() = 0;
    virtual ~ISpotifyMetadataAllocated() {}
};

class ISpotifyTrackObserver
{
public:
    virtual void MetadataChanged(Media::ISpotifyMetadataAllocated* aMetadata) = 0;
    /*
     * Should be called when track offset has actively changed (e.g., due to a
     * seek).
     */
    virtual void TrackOffsetChanged(TUint aOffsetMs) = 0;
    /*
     * Should be called to update current playback pos, so that action can be
     * taken if loss of sync detected.
     */
    virtual void TrackPosition(TUint aPositionMs) = 0;
    //virtual void FlushTrackState() = 0;
    virtual ~ISpotifyTrackObserver() {}
};

class SpotifyDidlLiteWriter : private INonCopyable
{
public:
    SpotifyDidlLiteWriter(const Brx& aUri, const ISpotifyMetadata& aMetadata);
    void Write(IWriter& aWriter, TUint aBitDepth, TUint aChannels, TUint aSampleRate) const;
protected:
    const BwsTrackUri iUri;
    const ISpotifyMetadata& iMetadata;
};

/**
 * Helper class to store start offset expressed in milliseconds or samples.
 * Each call to either of the Set() methods overwrites any value set (be it in
 * milliseconds or samples) in a previous call.
 */
class StartOffset
{
public:
    StartOffset();
    void SetMs(TUint aOffsetMs);
    TUint64 OffsetSample(TUint aSampleRate) const;
    TUint OffsetMs() const;
    TUint AbsoluteDiff(TUint aOffsetMs) const;
private:
    TUint iOffsetMs;
};

/*
 * Element to report number of samples seen since last MsgMode.
 */
class SpotifyReporter : public PipelineElement, public IPipelineElementUpstream, public ISpotifyReporter, public ISpotifyTrackObserver, private INonCopyable
{
private:
    static const TUint kSupportedMsgTypes;
    static const TUint kTrackOffsetChangeThresholdMs = 2000;
    static const Brn kInterceptMode;
public:
    SpotifyReporter(
        IPipelineElementUpstream& aUpstreamElement,
        MsgFactory& aMsgFactory,
        TrackFactory& aTrackFactory
    );
    ~SpotifyReporter();
public: // from IPipelineElementUpstream
    Msg* Pull() override;
public: // from ISpotifyReporter
    void AddSpotifyPlaybackObserver(ISpotifyPlaybackObserver& aObserver) override;
    TUint64 SubSamples() const override;
    void GetPlaybackPosMs(TUint& aStreamId, TUint& aPos) override;
    void Flush(TUint aFlushId) override;
public: // from ISpotifyTrackObserver
    void MetadataChanged(Media::ISpotifyMetadataAllocated* aMetadata) override;
    void TrackOffsetChanged(TUint aOffsetMs) override;
    void TrackPosition(TUint aPositionMs) override;
    //void FlushTrackState() override;
private: // PipelineElement
    Msg* ProcessMsg(MsgMode* aMsg) override;
    Msg* ProcessMsg(MsgTrack* aMsg) override;
    Msg* ProcessMsg(MsgDecodedStream* aMsg) override;
    Msg* ProcessMsg(MsgAudioPcm* aMsg) override;
    Msg* ProcessMsg(MsgFlush* aMsg) override;
private:
    void ClearDecodedStream();
    void UpdateDecodedStream(MsgDecodedStream& aMsg);
    TUint64 TrackLengthJiffiesLocked() const;
    MsgDecodedStream* CreateMsgDecodedStreamLocked() const;
    TUint GetPlaybackPosMsLocked() const;
private:
    IPipelineElementUpstream& iUpstreamElement;
    MsgFactory& iMsgFactory;
    TrackFactory& iTrackFactory;
    StartOffset iStartOffset;
    TUint iTrackDurationMs; // Track duration reported via out-of-band metadata messages.
    BwsTrackUri iTrackUri;
    ISpotifyMetadataAllocated* iMetadata;
    TBool iMsgDecodedStreamPending;
    MsgDecodedStream* iDecodedStream;
    TUint64 iSubSamples;
    TUint64 iSubSamplesTrack;
    TUint iStreamId;
    TUint iTrackDurationMsDecodedStream; // Track duration reported in-band via MsgDecodedStream.
    TBool iInterceptMode;
    TBool iPipelineTrackSeen;
    TBool iGeneratedTrackPending;
    TUint iPendingFlushId;
    TBool iPlaybackStartPending;
    TBool iPlaybackContinuePending;
    std::vector<std::reference_wrapper<ISpotifyPlaybackObserver>> iPlaybackObservers;
    mutable Mutex iLock;
};

} // namespace Media
} // namespace OpenHome
