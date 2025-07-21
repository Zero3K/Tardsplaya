#pragma once
// DirectShow Filter GUIDs for Tardsplaya Discontinuity Handler
// These GUIDs uniquely identify the filter components for COM registration

#include <windows.h>
#include <initguid.h>

// {E7B8C5A1-2F4D-4B8E-9A1C-3D6F8E9B4C2A}
DEFINE_GUID(CLSID_TardsplayaDiscontinuityFilter, 
    0xe7b8c5a1, 0x2f4d, 0x4b8e, 0x9a, 0x1c, 0x3d, 0x6f, 0x8e, 0x9b, 0x4c, 0x2a);

// {B4C2A1E7-8E9B-4D6F-A3C1-9B8E4F2D5A6B}
DEFINE_GUID(CLSID_TardsplayaSourcePin,
    0xb4c2a1e7, 0x8e9b, 0x4d6f, 0xa3, 0xc1, 0x9b, 0x8e, 0x4f, 0x2d, 0x5a, 0x6b);

// Custom media type for transport stream with discontinuity info
// {C5A4B3E2-9F8D-4A7B-B1E6-5C9A8D4F2B3E}
DEFINE_GUID(MEDIATYPE_TardsplayaTransportStream,
    0xc5a4b3e2, 0x9f8d, 0x4a7b, 0xb1, 0xe6, 0x5c, 0x9a, 0x8d, 0x4f, 0x2b, 0x3e);

// Transport stream subtype with frame numbering
// {D6B5A4F3-A1C8-5B9E-C2F7-6D8B9E5A3C4D}
DEFINE_GUID(MEDIASUBTYPE_TardsplayaFrameTaggedTS,
    0xd6b5a4f3, 0xa1c8, 0x5b9e, 0xc2, 0xf7, 0x6d, 0x8b, 0x9e, 0x5a, 0x3c, 0x4d);

// Filter category - Custom DirectShow Source Filters
// Using standard CLSID_LegacyAmFilterCategory for broad compatibility
#define FILTER_CATEGORY CLSID_LegacyAmFilterCategory

// Filter name and description
#define TARDSPLAYA_FILTER_NAME L"Tardsplaya Discontinuity Handler"
#define TARDSPLAYA_FILTER_DESCRIPTION L"DirectShow source filter for handling stream discontinuities using Tardsplaya transport stream engine"
#define TARDSPLAYA_PIN_NAME L"Transport Stream Output"

// Version information
#define TARDSPLAYA_FILTER_VERSION_MAJOR 1
#define TARDSPLAYA_FILTER_VERSION_MINOR 0
#define TARDSPLAYA_FILTER_VERSION_BUILD 0
#define TARDSPLAYA_FILTER_VERSION_REVISION 1