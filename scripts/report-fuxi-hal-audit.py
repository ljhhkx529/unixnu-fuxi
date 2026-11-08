#!/usr/bin/env python3
"""List what the audio plug-in implements, and which of it the 21H461 audit has to confirm on the device.

The plug-in is sheng's mechanism with fuxi's identity, so what it says to the audio server is known from the source.
This greps the source for the driver-interface functions, the property selectors and the IO operations, and marks the
ones whose meaning belongs to the audio server rather than to us: those are the audit's subject. If a selector on that
list disappears from the code the audit table is out of date, so this fails rather than quietly reporting less.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / "hal/FuxiAudioPlugIn.c"

# The selectors and calls whose semantics belong to the audio server: what they mean, not what we wish they meant.
SENSITIVE = {
    "kAudioServerPlugInDriverInterfaceUUID": "the driver interface version the server hands us must be the one we answer",
    "kAudioServerPlugInTypeUUID": "the plug-in factory's type must match what the server loads",
    "GetZeroTimeStamp": "fHostTime and fSampleTime are the whole timeline; the period they are quoted at is the server's",
    "kAudioDevicePropertyZeroTimeStampPeriod": "the server's expected anchor period, in frames",
    "kAudioDevicePropertySafetyOffset": "frames of slack the server assumes; sets the usable lead",
    "kAudioDevicePropertyLatency": "frames of latency the server assumes when it schedules",
    "kAudioDevicePropertyClockAlgorithm": "the algorithm we claim; the server may refuse one it does not know",
    "kAudioDevicePropertyClockDomain": "the clock domain the device is grouped in",
    "kAudioDevicePropertyClockIsStable": "what we promise about the clock's stability",
    "kAudioDevicePropertyTransportType": "the transport we claim decides how the device is presented",
    "kAudioDevicePropertyNominalSampleRate": "the rate the server schedules at must equal the page's",
    "kAudioStreamPropertyPhysicalFormat": "the format the hardware is said to have",
    "kAudioStreamPropertyVirtualFormat": "the format the server converts to; a mismatch here is silence",
    "kAudioStreamPropertyAvailablePhysicalFormats": "the set the server may choose from",
    "kAudioStreamPropertyAvailableVirtualFormats": "the set the server may choose from",
    "kAudioLevelControlPropertyScalarValue": "the volume the server sets; must be clamped by the amplifier's safe range",
    "kAudioLevelControlPropertyDecibelRange": "the range the server shows, which is not the amplifier's maximum",
    "kAudioLevelControlPropertyConvertScalarToDecibels": "the conversion the server uses to show it",
    "kAudioDevicePropertyPreferredChannelLayout": "the layout the server maps channels through",
    "kAudioDevicePropertyPreferredChannelsForStereo": "which two channels stereo uses",
    "kAudioDeviceUnsupportedFormatError": "what the server does when we refuse a format",
    "kAudioServerPlugInIOOperationWriteMix": "the buffer list the server hands us for playback",
    "kAudioServerPlugInIOOperationReadInput": "the buffer list the server hands us for capture",
    "kAudioStreamPropertyTerminalType": "how the device is named to the user",
}

DRIVER_FUNCTIONS = ("QueryInterface", "AddRef", "Release", "Initialize", "CreateDevice", "DestroyDevice",
                    "StartIO", "StopIO", "GetZeroTimeStamp", "WillDoIOOperation", "BeginIOOperation",
                    "DoIOOperation", "EndIOOperation", "GetPropertyDataSize", "GetPropertyData",
                    "SetPropertyData", "HasProperty")


def main() -> int:
    if not PLUGIN.is_file():
        print(f"FAIL {PLUGIN} is missing")
        return 1
    text = PLUGIN.read_text()
    functions = [f for f in DRIVER_FUNCTIONS if re.search(rf"\b{f}\b", text)]
    selectors = sorted(set(re.findall(r"\bkAudio[A-Za-z]+", text)))
    operations = sorted(set(re.findall(r"\bkAudioServerPlugInIOOperation[A-Za-z]+", text)))

    missing = [name for name in SENSITIVE if name not in functions and name not in selectors]
    print("| 项 | 类别 | 21H461 上要确认的事 |")
    print("| --- | --- | --- |")
    for name, why in SENSITIVE.items():
        kind = "驱动函数" if name in functions else "属性/操作"
        state = "" if name in functions or name in selectors else " **（源码里已不存在）**"
        print(f"| `{name}`{state} | {kind} | {why} |")
    print()
    print(f"插件实现 {len(functions)} 个驱动函数、{len(selectors)} 个属性选择器、{len(operations)} 个 IO 操作；"
          f"其中 {len(SENSITIVE)} 项属于音频服务器的语义，是审计的对象。")
    if missing:
        for name in missing:
            print(f"FAIL {name} is in the audit list but no longer in the plug-in: revisit the table")
        return 1
    print("PASS fuxi audio hal audit: the table and the plug-in agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
