#!/usr/bin/env python3
"""Validate the source-confirmed fuxi audio facts without claiming device execution."""

from pathlib import Path
import sys
import tomllib


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "facts/audio/manifest.toml"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL audio facts: {message}")


def main() -> int:
    with MANIFEST.open("rb") as stream:
        facts = tomllib.load(stream)

    require(facts.get("schema_version") == 1, "unsupported schema")
    require(facts.get("board") == "fuxi" and facts.get("soc") == "sm8550",
            "wrong board or SoC")
    require(facts.get("status") == "source-confirmed",
            "the manifest must not claim runtime verification")
    source = facts.get("source", {})
    require(len(source.get("revision", "")) == 40, "source revision is not pinned")
    require(source.get("kind") == "fixed-fuxi-source", "source kind is not explicit")

    bus = facts.get("speaker_bus", {})
    require(bus.get("controller") == "qupv3_se8_i2c", "unexpected amplifier bus")
    require(bus.get("mmio_base") == 0x00880000, "unexpected amplifier bus base")
    require((bus.get("sda_gpio"), bus.get("scl_gpio")) == (0, 1),
            "unexpected amplifier I2C pins")
    require(bus.get("runtime_verified") is False, "bus is not device-verified")

    amplifiers = facts.get("amplifier", [])
    require(len(amplifiers) == 2, "fuxi has exactly two declared amplifiers")
    expected = {
        "B": (0x40, 183, 99),
        "T": (0x42, 182, 98),
    }
    require({amp.get("role") for amp in amplifiers} == set(expected),
            "amplifier roles must be B and T")
    require(len({amp.get("i2c_address") for amp in amplifiers}) == 2,
            "amplifier I2C addresses must be distinct")
    require(len({amp.get("reset_gpio") for amp in amplifiers}) == 2,
            "fuxi amplifiers have independent reset GPIOs")
    for amp in amplifiers:
        role = amp["role"]
        require(amp.get("compatible") == "cirrus,cs35l41",
                f"{role} must be CS35L41, not the sheng CS35L43")
        require((amp.get("i2c_address"), amp.get("reset_gpio"), amp.get("irq_gpio"))
                == expected[role], f"{role} wiring does not match the pinned fuxi source")
        require(amp.get("boost_peak_milliamp") == 4000, f"{role} boost peak changed")
        require(amp.get("runtime_verified") is False, f"{role} is not device-verified")

    link = facts.get("speaker_link", {})
    require((link.get("clock_gpio"), link.get("word_select_gpio"),
             link.get("data_input_gpio"), link.get("data_output_gpio"))
            == (121, 123, 122, 124), "unexpected secondary TDM pin assignment")
    require(link.get("max_slots") == 4, "unexpected maximum TDM slots")
    for key in ("soc_is_clock_master", "sample_rate", "sample_width",
                "slot_width", "slot_masks"):
        require(link.get(key) == "unverified", f"{key} requires runtime evidence")

    codec = facts.get("codec", {})
    require(codec.get("compatible") == "qcom,wcd938x-codec", "unexpected codec")
    require(codec.get("tx_soundwire_mmio") == 0x06D30000,
            "unexpected TX SoundWire base")
    require(codec.get("reset_gpio") == 108, "unexpected codec reset GPIO")
    require(codec.get("micbias_millivolt") == [2750] * 4,
            "unexpected mic-bias declaration")

    mic = facts.get("microphone_route", {})
    for key in ("primary_mic", "micbias", "adc", "soundwire_slave_port",
                "soundwire_channel", "master_port", "tx_decimator",
                "codec_dma_endpoint"):
        require(mic.get(key) == "unverified", f"microphone {key} must remain unverified")

    adsp = facts.get("adsp", {})
    for key in ("firmware_source", "firmware_hash", "carveout"):
        require(adsp.get(key) == "unverified", f"ADSP {key} requires fuxi evidence")

    safety = facts.get("safety", {})
    require(safety.get("default_amplifier_state") == "reset-or-global-mute",
            "amplifiers must default safe")
    for key in ("allow_high_power_without_protection",
                "allow_cross_device_calibration", "allow_cs35l43_register_program"):
        require(safety.get(key) is False, f"unsafe policy enabled: {key}")
    require(0 < safety.get("first_tone_max_milliseconds", 0) <= 500,
            "first-tone duration exceeds the bring-up bound")

    print("PASS fuxi audio facts: 2 CS35L41 amps, TDM/WCD wiring pinned; runtime fields unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
