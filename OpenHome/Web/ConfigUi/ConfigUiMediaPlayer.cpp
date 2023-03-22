#include <OpenHome/Web/ConfigUi/ConfigUiMediaPlayer.h>
#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>
#include <OpenHome/Web/ConfigUi/FileResourceHandler.h>
#include <OpenHome/Av/VolumeManager.h>
#include <OpenHome/Av/RebootHandler.h>
#include <OpenHome/Av/Qobuz/Qobuz.h>

#include <vector>

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Web;


ConfigAppMediaPlayer::ConfigAppMediaPlayer(IInfoAggregator& aInfoAggregator,
                                           Environment& aEnv,
                                           Product& aProduct,
                                           Configuration::IConfigManager& aConfigManager,
                                           IConfigAppResourceHandlerFactory& aResourceFactory,
                                           const std::vector<const Brx*>& aSources,
                                           const Brx& aResourcePrefix, const Brx& aResourceDir,
                                           TUint aResourceHandlerCount, TUint aMaxTabs, TUint aSendQueueSize,
                                           TUint aMsgBufCount, TUint aMsgBufBytes,
                                           IRebootHandler& aRebootHandler)
    : ConfigAppSources(aInfoAggregator, aConfigManager,
                       aResourceFactory, aSources, aResourcePrefix, aResourceDir,
                       aResourceHandlerCount, aMaxTabs, aSendQueueSize,
                       aMsgBufCount, aMsgBufBytes, aRebootHandler)
{
    AddValue(new ConfigUiValRoModelIcon(aProduct));
    AddValue(new ConfigUiValRoModelName(aProduct));
    AddValue(new ConfigUiValRoModelUrl(aProduct));
    AddValue(new ConfigUiValRoManufacturerName(aProduct));
    AddValue(new ConfigUiValRoManufacturerUrl(aProduct));
    AddValue(new ConfigUiValRoIpAddress(aEnv.NetworkAdapterList()));

    AddConfigNumConditional(Brn("Sender.Channel"));
    AddConfigNumConditional(Brn("Sender.Preset"));
    AddConfigNumConditional(VolumeConfig::kKeyBalance);
    AddConfigNumConditional(VolumeConfig::kKeyLimit);
    AddConfigNumConditional(VolumeConfig::kKeyStartupValue);
    AddConfigChoiceConditional(VolumeConfig::kKeyStartupEnabled);

    AddConfigChoiceConditional(Brn("Device.AutoPlay"));
    AddConfigChoiceConditional(Brn("Sender.Enabled"));
    AddConfigChoiceConditional(Brn("Sender.Mode"));
    AddConfigChoiceConditional(Brn("Source.NetAux.Auto"));
    AddConfigChoiceConditional(Qobuz::kConfigKeySoundQuality);
    AddConfigChoiceConditional(Brn("qobuz.com.Enabled"));
    AddConfigChoiceConditional(Brn("tidalhifi.com.SoundQuality"));
    AddConfigChoiceConditional(Brn("tidalhifi.com.Enabled"));
    AddConfigChoiceConditional(Brn("tunein.com.Enabled"));
    AddConfigTextConditional(Brn("Radio.TuneInUserName"));
    AddConfigChoiceConditional(Brn("Roon.Protocol"));
}
