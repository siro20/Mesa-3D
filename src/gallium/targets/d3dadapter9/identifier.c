/*
 * Copyright 2015 Patrick Rudolph <siro@das-labor.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "adapter9.h"

#include "xmlconfig.h"
#include "xmlpool.h"

#include <stdio.h>

#define DBG_CHANNEL DBG_ADAPTER

#define VERSION_DWORD(hi, lo) \
    ((DWORD)( \
        ((DWORD)((hi) & 0xFFFF) << 16) | \
         (DWORD)((lo) & 0xFFFF) \
    ))

/* Regarding os versions, we should not define our own as that would simply be
 * weird. Defaulting to Win2k/XP seems sane considering the origin of D3D9. The
 * driver also defaults to being a generic D3D9 driver, which of course only
 * matters if you're actually using the DDI. */
#define VERSION_HIGH    VERSION_DWORD(0x0006, 0x000E) /* winxp, d3d9 */
#define VERSION_LOW     VERSION_DWORD(0x0000, 0x0001) /* version, build */

void d3d_fill_driver(const char* name, D3DADAPTER_IDENTIFIER9* drvid);

/* this is taken from WINE (https://www.winehq.org/)
 * ecspecialy wined3d_private.h and directx.c */
enum d3d_pci_vendor
{
    HW_VENDOR_SOFTWARE              = 0x0000,
    HW_VENDOR_AMD                   = 0x1002,
    HW_VENDOR_NVIDIA                = 0x10de,
    HW_VENDOR_VMWARE                = 0x15ad,
    HW_VENDOR_INTEL                 = 0x8086,
};

struct mesa_win_names {
    const char* mesa;
    const char* device_name;
    const char* driver_name;
    WORD version;                       /* version word ('y'), contained in low word of DriverVersion.HighPart */
    WORD subversion;                    /* subversion word ('z'), contained in high word of DriverVersion.LowPart */
    WORD build;                         /* build number ('w'), contained in low word of DriverVersion.LowPart */
};

static const struct mesa_win_names amd_cards[] =
{
    /* Sea Islands */
    {"HAWAII",                      "AMD Radeon R9 290"             , "atiumdag.dll", 17, 10, 1280},
    {"KAVERI",                      "AMD Radeon(TM) R7 Graphics"    , "atiumdag.dll", 17, 10, 1280},
    {"KABINI",                      "AMD Radeon HD 8400 / R3 Series", "atiumdag.dll", 17, 10, 1280},
    {"BONAIRE",                     "AMD Radeon HD 8770"            , "atiumdag.dll", 17, 10, 1280},
    /* Southern Islands */
    {"OLAND",                       "AMD Radeon HD 8670"        , "atiumdag.dll", 17, 10, 1280},
    {"HAINAN",                      "AMD Radeon HD 8600M Series", "atiumdag.dll", 17, 10, 1280},
    {"TAHITI",                      "AMD Radeon HD 7900 Series" , "atiumdag.dll", 17, 10, 1280},
    {"PITCAIRN",                    "AMD Radeon HD 7800 Series" , "atiumdag.dll", 17, 10, 1280},
    {"CAPE VERDE",                  "AMD Radeon HD 7700 Series" , "atiumdag.dll", 17, 10, 1280},
    /* Northern Islands */
    {"ARUBA",                       "AMD Radeon HD 7660D"               , "atiumdag.dll", 17, 10, 1280},
    {"CAYMAN",                      "AMD Radeon HD 6900 Series"         , "atiumdag.dll", 17, 10, 1280},
    {"BARTS",                       "AMD Radeon HD 6800 Series"         , "atiumdag.dll", 17, 10, 1280},
    {"TURKS",                       "AMD Radeon HD 6600 Series"         , "atiumdag.dll", 17, 10, 1280},
    /* SUMO2 first, because we do a strstr(). */
    {"SUMO2",                       "AMD Radeon HD 6410D"               , "atiumdag.dll", 17, 10, 1280},
    {"SUMO",                        "AMD Radeon HD 6550D"               , "atiumdag.dll", 17, 10, 1280},
    {"CAICOS",                      "AMD Radeon HD 6400 Series"         , "atiumdag.dll", 17, 10, 1280},
    {"PALM",                        "AMD Radeon HD 6300 series Graphics", "atiumdag.dll", 17, 10, 1280},
    /* Evergreen */
    {"HEMLOCK",                     "ATI Radeon HD 5900 Series", "atiumdag.dll", 17, 10, 1280},
    {"CYPRESS",                     "ATI Radeon HD 5800 Series", "atiumdag.dll", 17, 10, 1280},
    {"JUNIPER",                     "ATI Radeon HD 5700 Series", "atiumdag.dll", 17, 10, 1280},
    {"REDWOOD",                     "ATI Radeon HD 5600 Series", "atiumdag.dll", 17, 10, 1280},
    {"CEDAR",                       "ATI Radeon HD 5400 Series", "atiumdag.dll", 17, 10, 1280},
    /* R700 */
    {"R700",                        "ATI Radeon HD 4800 Series", "atiumdag.dll", 17, 10, 1280},
    {"RV790",                       "ATI Radeon HD 4800 Series", "atiumdag.dll", 17, 10, 1280},
    {"RV770",                       "ATI Radeon HD 4800 Series", "atiumdag.dll", 17, 10, 1280},
    {"RV740",                       "ATI Radeon HD 4700 Series", "atiumdag.dll", 17, 10, 1280},
    {"RV730",                       "ATI Radeon HD 4600 Series", "atiumdag.dll", 17, 10, 1280},
    {"RV710",                       "ATI Radeon HD 4350", "atiumdag.dll", 17, 10, 1280},
    /* R600/R700 integrated */
    {"RS880",                       "ATI Mobility Radeon HD 4200", "atiumdag.dll", 17, 10, 1280},
    {"RS780",                       "ATI Radeon HD 3200 Graphics", "atiumdag.dll", 17, 10, 1280},
    /* R600 */
    {"R680",                        "ATI Radeon HD 2900 XT", "atiumdag.dll", 17, 10, 1280},
    {"R600",                        "ATI Radeon HD 2900 XT", "atiumdag.dll", 17, 10, 1280},
    {"RV670",                       "ATI Radeon HD 2900 XT", "atiumdag.dll", 17, 10, 1280},
    {"RV635",                       "ATI Mobility Radeon HD 2600", "atiumdag.dll", 17, 10, 1280},
    {"RV630",                       "ATI Mobility Radeon HD 2600", "atiumdag.dll", 17, 10, 1280},
    {"RV620",                       "ATI Mobility Radeon HD 2350", "atiumdag.dll", 17, 10, 1280},
    {"RV610",                       "ATI Mobility Radeon HD 2350", "atiumdag.dll", 17, 10, 1280},
    /* R500 */
    {"R580",                        "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"R520",                        "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"RV570",                       "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"RV560",                       "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"RV535",                       "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"RV530",                       "ATI Radeon X1600 Series", "atiumdag.dll", 14, 10, 741},
    {"RV516",                       "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"RV515",                       "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    /* R400 */
    {"R481",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"R480",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"R430",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"R423",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"R420",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"R410",                        "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    {"RV410",                       "ATI Radeon X700 SE", "atiumdag.dll", 14, 10, 741},
    /* Radeon Xpress - onboard, DX9b, Shader 2.0, 300-400 MHz */
    {"RS740",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS690",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS600",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS485",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS482",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS480",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    {"RS400",                       "ATI RADEON XPRESS 200M Series", "atiumdag.dll", 14, 10, 741},
    /* R300 */
    {"R360",                        "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"R350",                        "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"R300",                        "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"RV370",                       "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"RV360",                       "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"RV351",                       "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
    {"RV350",                       "ATI Radeon 9500", "atiumdag.dll", 14, 10, 741},
};

static const struct mesa_win_names nvidia_cards[] =
{
    {"NV124",                       "NVIDIA GeForce GTX 970", "nvd3dum.dll",  18, 13, 4052},
    {"NV117",                       "NVIDIA GeForce GTX 750", "nvd3dum.dll",  18, 13, 4052},
    /* Kepler */
    {"NVF1",                        "NVIDIA GeForce GTX 780 Ti", "nvd3dum.dll",  18, 13, 4052},
    {"NVF0",                        "NVIDIA GeForce GTX 780",    "nvd3dum.dll",  18, 13, 4052},
    {"NVE6",                        "NVIDIA GeForce GTX 770M",   "nvd3dum.dll",  18, 13, 4052},
    {"NVE4",                        "NVIDIA GeForce GTX 680",    "nvd3dum.dll",  18, 13, 4052},
    /* Fermi */
    {"NVD9",                        "NVIDIA GeForce GT 520",  "nvd3dum.dll",  18, 13, 4052},
    {"NVCF",                        "NVIDIA GeForce GTX 550", "nvd3dum.dll",  18, 13, 4052},
    {"NVCE",                        "NVIDIA GeForce GTX 560", "nvd3dum.dll",  18, 13, 4052},
    {"NVC8",                        "NVIDIA GeForce GTX 570", "nvd3dum.dll",  18, 13, 4052},
    {"NVC4",                        "NVIDIA GeForce GTX 460", "nvd3dum.dll",  18, 13, 4052},
    {"NVC3",                        "NVIDIA GeForce GT 440",  "nvd3dum.dll",  18, 13, 4052},
    {"NVC1",                        "NVIDIA GeForce GT 420",  "nvd3dum.dll",  18, 13, 4052},
    {"NVC0",                        "NVIDIA GeForce GTX 480", "nvd3dum.dll",  18, 13, 4052},
    /* Tesla */
    {"NVAF",                        "NVIDIA GeForce 320M",     "nvd3dum.dll",  18, 13, 4052},
    {"NVAC",                        "NVIDIA GeForce 315M",     "nvd3dum.dll",  18, 13, 4052},
    {"NVAA",                        "NVIDIA GeForce 315M",     "nvd3dum.dll",  18, 13, 4052},
    {"NVA8",                        "NVIDIA GeForce 210",      "nvd3dum.dll",  18, 13, 4052},
    {"NVA5",                        "NVIDIA GeForce GT 220",   "nvd3dum.dll",  18, 13, 4052},
    {"NVA3",                        "NVIDIA GeForce GT 240",   "nvd3dum.dll",  18, 13, 4052},
    {"NVA0",                        "NVIDIA GeForce GTX 280",  "nvd3dum.dll",  18, 13, 4052},
    {"NV98",                        "NVIDIA GeForce 9200",     "nvd3dum.dll",  18, 13, 4052},
    {"NV96",                        "NVIDIA GeForce 9400 GT",  "nvd3dum.dll",  18, 13, 4052},
    {"NV94",                        "NVIDIA GeForce 9600 GT",  "nvd3dum.dll",  18, 13, 4052},
    {"NV92",                        "NVIDIA GeForce 9800 GT",  "nvd3dum.dll",  18, 13, 4052},
    {"NV86",                        "NVIDIA GeForce 8500 GT",  "nvd3dum.dll",  18, 13, 4052},
    {"NV84",                        "NVIDIA GeForce 8600 GT",  "nvd3dum.dll",  18, 13, 4052},
    {"NV50",                        "NVIDIA GeForce 8800 GTX", "nvd3dum.dll",  18, 13, 4052},
    /* Curie */
    {"NV68",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},      /* 7050 */
    {"NV67",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},      /* 7000M */
    {"NV63",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},      /* 7100 */
    {"NV4E",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},      /* 6100 Go / 6150 Go */
    {"NV4C",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},      /* 6150SE */
    {"NV4B",                        "NVIDIA GeForce 7600 GT", "nvd3dum.dll",  18, 13,  783},
    {"NV4A",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},
    {"NV49",                        "NVIDIA GeForce 7800 GT", "nvd3dum.dll",  18, 13,  783},    /* 7900 */
    {"NV47",                        "NVIDIA GeForce 7800 GT", "nvd3dum.dll",  18, 13,  783},
    {"NV46",                        "NVIDIA GeForce Go 7400", "nvd3dum.dll",  18, 13,  783},
    {"NV45",                        "NVIDIA GeForce 6800",    "nvd3dum.dll",  18, 13,  783},
    {"NV44",                        "NVIDIA GeForce 6200",    "nvd3dum.dll",  18, 13,  783},
    {"NV43",                        "NVIDIA GeForce 6600 GT", "nvd3dum.dll",  18, 13,  783},
    {"NV42",                        "NVIDIA GeForce 6800",    "nvd3dum.dll",  18, 13,  783},
    {"NV41",                        "NVIDIA GeForce 6800",    "nvd3dum.dll",  18, 13,  783},
    {"NV40",                        "NVIDIA GeForce 6800",    "nvd3dum.dll",  18, 13,  783},
    /* Rankine */
    {"NV38",                        "NVIDIA GeForce FX 5800", "nv4_disp.dll", 14, 11, 7516},    /* FX 5950 Ultra */
    {"NV36",                        "NVIDIA GeForce FX 5800", "nv4_disp.dll", 14, 11, 7516},    /* FX 5700/5750 */
    {"NV35",                        "NVIDIA GeForce FX 5800", "nv4_disp.dll", 14, 11, 7516},    /* FX 5900 */
    {"NV34",                        "NVIDIA GeForce FX 5200", "nv4_disp.dll", 14, 11, 7516},
    {"NV31",                        "NVIDIA GeForce FX 5600", "nv4_disp.dll", 14, 11, 7516},
    {"NV30",                        "NVIDIA GeForce FX 5800", "nv4_disp.dll", 14, 11, 7516},
    /* Kelvin */
    {"nv28",                        "NVIDIA GeForce4 Ti 4200", "nv4_disp.dll", 14, 10, 9371},
    {"nv25",                        "NVIDIA GeForce4 Ti 4200", "nv4_disp.dll", 14, 10, 9371},
    {"nv20",                        "NVIDIA GeForce3",         "nv4_disp.dll", 14, 10, 9371},
    /* Celsius */
    {"nv1F",                        "NVIDIA GeForce4 MX 460",           "nv4_disp.dll", 14, 10, 9371},       /* GF4 MX IGP */
    {"nv1A",                        "NVIDIA GeForce2 GTS/GeForce2 Pro", "nv4_disp.dll", 14, 10, 7186},          /* GF2 IGP */
    {"nv18",                        "NVIDIA GeForce4 MX 460",           "nv4_disp.dll", 14, 10, 9371},
    {"nv17",                        "NVIDIA GeForce4 MX 460",           "nv4_disp.dll", 14, 10, 9371},
    {"nv16",                        "NVIDIA GeForce2 GTS/GeForce2 Pro", "nv4_disp.dll", 14, 10, 7186},
    {"nv15",                        "NVIDIA GeForce2 GTS/GeForce2 Pro", "nv4_disp.dll", 14, 10, 7186},
    {"nv11",                        "NVIDIA GeForce2 MX/MX 400",        "nv4_disp.dll", 14, 10, 9371},
    {"nv10",                        "NVIDIA GeForce 256",               "nv4_disp.dll", 14, 10, 7186},
    /* Fahrenheit */
    {"nv05",                        "NVIDIA RIVA TNT2/TNT2 Pro", "nv4_disp.dll", 14, 10, 7186},
    {"nv04",                        "NVIDIA RIVA TNT",           "nv4_disp.dll", 14, 10, 7186},
};

static const struct mesa_win_names vmware_cards[] =
{
    {"SVGA3D",             "VMware SVGA 3D (Microsoft Corporation - WDDM)", "vm3dum.dll",   14, 1,  1134},
};

static const struct mesa_win_names intel_cards[] =
{
    /* Haswell */
    {"Haswell Mobile",              "Intel(R) Haswell Mobile",  "igdumd32.dll", 15, 10, 1666},
    /* Ivybridge */
    {"Ivybridge Server",            "Intel(R) Ivybridge Server",   "igdumd32.dll", 15, 10, 1666},
    {"Ivybridge Mobile",            "Intel(R) Ivybridge Mobile",   "igdumd32.dll", 15, 10, 1666},
    {"Ivybridge Desktop",           "Intel(R) Ivybridge Desktop",  "igdumd32.dll", 15, 10, 1666},
    /* Sandybridge */
    {"Sandybridge Server",          "Intel(R) Sandybridge Server",   "igdumd32.dll", 15, 10, 1666},
    {"Sandybridge Mobile",          "Intel(R) Sandybridge Mobile",   "igdumd32.dll", 15, 10, 1666},
    {"Sandybridge Desktop",         "Intel(R) Sandybridge Desktop",  "igdumd32.dll", 15, 10, 1666},
    /* Ironlake */
    {"Ironlake Mobile",             "Intel(R) Ironlake Mobile",   "igdumd32.dll", 15, 10, 1666},
    {"Ironlake Desktop",            "Intel(R) Ironlake Desktop",  "igdumd32.dll", 15, 10, 1666},
    /* G4x */
    {"B43",                         "Intel(R) B43",                                "igdumd32.dll", 15, 10, 1666},
    {"G41",                         "Intel(R) G41",                                "igdumd32.dll", 15, 10, 1666},
    {"G45",                         "Intel(R) G45/G43",                            "igdumd32.dll", 15, 10, 1666},
    {"Q45",                         "Intel(R) Q45/Q43",                            "igdumd32.dll", 15, 10, 1666},
    {"Integrated Graphics Device",  "Intel(R) Integrated Graphics Device",         "igdumd32.dll", 15, 10, 1666},
    {"GM45",                        "Mobile Intel(R) GM45 Express Chipset Family", "igdumd32.dll", 15, 10, 1666},
    /* i965 */
    {"965GME",                      "Intel(R) 965GME",  "igdumd32.dll", 15, 10, 1666},
    {"965GM",                       "Mobile Intel(R) 965 Express Chipset Family",  "igdumd32.dll", 15, 10, 1666},
    {"X3100",                       "Mobile Intel(R) 965 Express Chipset Family",  "igdumd32.dll", 15, 10, 1666},  /* MacOS */
    {"946GZ",                       "Intel(R) 946GZ",                              "igdumd32.dll", 15, 10, 1666},
    {"965G",                        "Intel(R) 965G",                               "igdumd32.dll", 15, 10, 1666},
    {"965Q",                        "Intel(R) 965Q",                               "igdumd32.dll", 15, 10, 1666},
    /* i945 */
    {"Pineview M",                  "Intel(R) IGD",                                  "igdumd32.dll", 14, 10, 1504},
    {"Pineview G",                  "Intel(R) IGD",                                  "igdumd32.dll", 14, 10, 1504},
    {"IGD",                         "Intel(R) IGD",                                  "igdumd32.dll", 14, 10, 1504},
    {"Q33",                         "Intel(R) G33",                                  "igdumd32.dll", 14, 10, 1504},
    {"G33",                         "Intel(R) G33",                                  "igdumd32.dll", 14, 10, 1504},
    {"Q35",                         "Intel(R) Q35",                                  "igdumd32.dll", 14, 10, 1504},
    {"945GME",                      "Intel(R) 945GME",                               "igdumd32.dll", 14, 10, 1504},
    {"945GM",                       "Mobile Intel(R) 945GM Express Chipset Family",  "igdumd32.dll", 14, 10, 1504},
    {"GMA 950",                     "Mobile Intel(R) 945GM Express Chipset Family",  "igdumd32.dll", 14, 10, 1504},  /* MacOS */
    {"945G",                        "Intel(R) 945G",                                 "igdumd32.dll", 14, 10, 1504},
    /* i915 */
    {"915GM",                       "Mobile Intel(R) 915GM/GMS,910GML Express Chipset Family",  "igxprd32.dll", 14, 10, 4764},
    {"E7221G",                      "Intel(R) E7221G",                                          "igxprd32.dll", 14, 10, 4764},
    {"915G",                        "Intel(R) 82915G/GV/910GL Express Chipset Family",          "igxprd32.dll", 14, 10, 4764},
    /* i8xx */
    {"865G",                        "Intel(R) 82865G Graphics Controller",              "ialmrnt5.dll", 14, 10, 3889},
    {"845G",                        "Intel(R) 845G",                                    "ialmrnt5.dll", 14, 10, 3889},
    {"855GM",                       "Intel(R) 82852/82855 GM/GME Graphics Controller",  "ialmrnt5.dll", 14, 10, 3889},
    {"830M",                        "Intel(R) 82830M Graphics Controller",              "ialmrnt5.dll", 14, 10, 3889},
};

#define FALLBACK_VENDOR_ID HW_VENDOR_NVIDIA
#define FALLBACK_DEVICE_ID 0x11C0

/* fill driver name, driver and driver version */
void d3d_fill_driver(const char* name, D3DADAPTER_IDENTIFIER9* drvid)
{
    int i, len = 0;
    const struct mesa_win_names *list = NULL;

    /* match a vendor, if no vendor is found, return a fake one */
    switch (drvid->VendorId)
    {
    case HW_VENDOR_INTEL:
        list = intel_cards;
        len = sizeof(intel_cards)/sizeof(intel_cards[0]);
        break;
    case HW_VENDOR_VMWARE:
        list = vmware_cards;
        len = sizeof(vmware_cards)/sizeof(vmware_cards[0]);
        break;
    case HW_VENDOR_AMD:
        list = amd_cards;
        len = sizeof(amd_cards)/sizeof(amd_cards[0]);
        break;
    case HW_VENDOR_NVIDIA:
        list = nvidia_cards;
        len = sizeof(nvidia_cards)/sizeof(nvidia_cards[0]);
        break;
    default:
        WARN_ONCE("Unknown vendor 0x%04X! Faking VendorId 0x%04X, DeviceId 0x%04X\n",
                drvid->VendorId, FALLBACK_VENDOR_ID, FALLBACK_DEVICE_ID);
        list = nvidia_cards;
        len = sizeof(nvidia_cards)/sizeof(nvidia_cards[0]);
        drvid->VendorId = FALLBACK_VENDOR_ID;
        drvid->DeviceId = FALLBACK_DEVICE_ID;
        break;
    }

    /* try to find a matching card */
    for(i=0;i<len;i++) {
        if (strstr(name, list[i].mesa)) {
            strncpy(drvid->Driver, list[i].driver_name, sizeof(drvid->Driver));
            strncpy(drvid->Description,  list[i].device_name, sizeof(drvid->Description));
            drvid->DriverVersionLowPart = VERSION_DWORD(list[i].subversion, list[i].build); /* minor, build */
            drvid->DriverVersionHighPart = VERSION_DWORD(6, list[i].version); /* OS, major */
            return;
        }
    }

    /* in case card is not found, return a fake card */
    strncpy(drvid->Driver, list[0].driver_name, sizeof(drvid->Driver));
    strncpy(drvid->Description,  list[0].device_name, sizeof(drvid->Description));
    drvid->DriverVersionLowPart = VERSION_DWORD(list[0].subversion, list[0].build); /* minor, build */
    drvid->DriverVersionHighPart = VERSION_DWORD(6, list[0].version); /* OS, major */
    WARN_ONCE("Unknown card VendorId 0x%04X, DeviceId 0x%04X.\n"
            "Please consider adding it to this list!\n"
            "Using fake card '%s'\n",
            drvid->VendorId, drvid->DeviceId, list[0].device_name);
}

