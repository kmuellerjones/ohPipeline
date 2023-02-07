#include "RamStore.h"
#include <OpenHome/Av/KvpStore.h>
#include <OpenHome/Private/Printer.h>

#include <map>

using namespace OpenHome;
using namespace OpenHome::Av;

RamStore::RamStore(const Brx& aImageFileName)
    : iImageFileName(aImageFileName)
{
}

RamStore::~RamStore() {}

void RamStore::LoadStaticData(IStoreLoaderStatic& aLoader)
{
    aLoader.AddStaticItem(StaticDataKey::kBufManufacturerName, "OpenHome");
    aLoader.AddStaticItem(StaticDataKey::kBufManufacturerInfo, "insert oh info here...");
    aLoader.AddStaticItem(StaticDataKey::kBufManufacturerUrl, "http://www.openhome.org");
    aLoader.AddStaticItem(StaticDataKey::kBufManufacturerImageUrl, "http://wiki.openhome.org/mediawiki/skins/openhome/images/logo.png");
    aLoader.AddStaticItem(StaticDataKey::kBufModelName, "OpenHome Media Player (test)");
    aLoader.AddStaticItem(StaticDataKey::kBufModelInfo, "Test implementation of ohMediaPlayer");
    aLoader.AddStaticItem(StaticDataKey::kBufModelUrl, "http://wiki.openhome.org/wiki/OhMedia");
    aLoader.AddStaticItem(StaticDataKey::kBufModelImageUrl, iImageFileName.CString());
    aLoader.AddStaticItem(StaticDataKey::kBufModelImageHiresUrl, iImageFileName.CString());
}
