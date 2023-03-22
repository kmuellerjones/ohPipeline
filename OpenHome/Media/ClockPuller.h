#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Media/Pipeline/Msg.h>

namespace OpenHome {
namespace Media {

class IClockPuller : public IPipelineBufferObserver
{
public:
    virtual ~IClockPuller() {}
    virtual void Start() = 0;
    virtual void Stop() = 0;
};

class IPullableClock
{
public:
    static const TUint kNominalFreq = 1u<<31;
public:
    virtual ~IPullableClock() {}
    /**
     * Signal that the clock should be pulled.
     *
     * @param[in] aMultiplier  Amount to pull by as fix 1.31 value (so, in the range [0..2)).
     *                         kNominalFreq implies no pull - i.e. run at nominal frequency.
     */
    virtual void PullClock(TUint aMultiplier) = 0;
    /**
     * Report the maximum deviation from nominal a particular clock supports.
     */
    virtual TUint MaxPull() const = 0;
};

class ClockPullerPipeline : public IClockPuller
{
public:
    ClockPullerPipeline(IClockPuller& aClockPullerPipeline);
    void SetClockPullerMode(Optional<IClockPuller> aClockPuller);
private: // from IClockPuller
    void Update(TInt aDelta) override;
    void Start() override;
    void Stop() override;
private:
    IClockPuller& iPipeline;
    IClockPuller* iMode;
};

class ClockPullerMock : public IClockPuller
{
private: // from IClockPuller
    void Update(TInt aDelta) override;
    void Start() override;
    void Stop() override;
};

} // namespace Media
} // namespace OpenHome
