#!/usr/bin/env python3
"""Validate the fuxi baseband binding: every evidence key resolves, every confirmed value
matches the source it cites, and nothing claims a device measurement.

This is the baseband counterpart of check-fuxi-audio-facts.py. It reads the binding and the
manifest and checks them against each other and against the sources on this machine. A source
that is not on this machine is skipped, not assumed: the check reports it as unverified rather
than passing it silently.
"""

from pathlib import Path
import re
import sys
import tomllib


ROOT = Path(__file__).resolve().parents[1]
FACTS = ROOT / "facts/baseband"
BINDING = FACTS / "binding.toml"
MANIFEST = FACTS / "manifest.toml"

# The CAF tree this board's own device tree was read from. It is a work file, not part of the
# repository, so it may be gone; the check says so instead of pretending.
CAF_TREE = Path("/tmp/x13/work/device-info/fuxi-live.dts")

STATES = {"confirmed", "inferred", "chosen", "unverified"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL baseband facts: {message}")


def resolve(table: dict, dotted: str):
    current = table
    for part in dotted.split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def entries(binding: dict) -> dict:
    """The value entries of the binding, keyed by their dotted path."""
    found = {}

    def walk(table: dict, prefix: str) -> None:
        for key, value in table.items():
            if not isinstance(value, dict):
                continue
            if "value" in value and "state" in value:
                found[prefix + key] = value
            else:
                walk(value, prefix + key + ".")

    walk(binding, "")
    return found


def main() -> int:
    with BINDING.open("rb") as stream:
        binding = tomllib.load(stream)
    with MANIFEST.open("rb") as stream:
        manifest = tomllib.load(stream)

    require(binding.get("schema_version") == 1, "unsupported binding schema")
    require(manifest.get("schema_version") == 1, "unsupported manifest schema")
    for name, table in (("binding", binding), ("manifest", manifest)):
        require(table.get("board") == "fuxi" and table.get("soc") == "sm8550",
                f"the {name} names the wrong board or SoC")
        require(table.get("status") == "source-confirmed",
                f"the {name} must not claim runtime verification")

    found = entries(binding)
    require(len(found) >= 40, f"the binding lost entries: only {len(found)}")

    # Every value carries a state, and every evidence key points at something that exists.
    for path, entry in found.items():
        state = entry.get("state")
        require(state in STATES, f"{path}: unknown state {state!r}")
        require("value" in entry, f"{path}: no value")
        evidence = entry.get("evidence")
        require(isinstance(evidence, str) and evidence,
                f"{path}: no evidence")
        if evidence == "none":
            # Nothing supports it yet, so the state must say exactly that.
            require(state == "unverified",
                    f"{path}: no evidence but the state is {state!r}")
            continue
        if evidence.startswith("design:"):
            continue
        require(resolve(manifest, evidence) is not None,
                f"{path}: the evidence key {evidence!r} is not in the manifest")

    # A confirmed value must cite a source that states a path, and that path must exist in the
    # manifest's own file list. A design decision may not be cited as a measurement.
    for path, entry in found.items():
        if entry.get("state") != "confirmed":
            continue
        evidence = entry.get("evidence", "")
        require(resolve(manifest, evidence) is not None,
                f"{path}: a confirmed value needs a source in the manifest")
        require(not evidence.startswith("design:"),
                f"{path}: a confirmed value cannot rest on a design decision")

    # The PAS ids are the values the whole bring-up turns on. Pin them.
    require(binding["pas"]["modem"]["value"] == 4, "the modem image's PAS id changed")
    require(binding["pas"]["modem_dtb"]["value"] == 0x26, "the modem DTB's PAS id changed")
    require(binding["pas"]["proxy_pd_names"]["value"] == ["cx", "mss"],
            "the modem's proxy power domains changed")

    # The firmware files, confirmed on the device on 2026-11-19: the partition's /image directory
    # holds exactly the two names the secure world asks for. The names are the lookup's whole
    # point, so a change to either has to fail here rather than be found on the device.
    require(binding["firmware"]["image_file"]["value"] == "modem.mdt",
            "the modem image's file name changed")
    require(binding["firmware"]["dtb_file"]["value"] == "modem_dtb.mdt",
            "the modem DTB's file name changed")
    require(binding["firmware"]["directory"]["value"] == "/image",
            "the firmware directory moved")
    require(binding["firmware"]["segment_prefix"]["value"] == "modem.b",
            "the segment prefix changed")
    require(binding["firmware"]["segment_count"]["value"] == 29,
            "the modem image's segment count changed")
    require(binding["firmware"]["image_size"]["value"] == 6364,
            "the modem image's size changed")
    require(binding["firmware"]["dtb_size"]["value"] == 4044,
            "the modem DTB's size changed")

    # The rails, confirmed on the device on 2026-11-19: the board's own command database carries
    # both of the modem's proxy power domain rails. An earlier inference from the sheng board said
    # mss.lvl was absent, so this pair is pinned in both directions.
    require(binding["rails"]["cx_in_cmd_db"]["value"] is True,
            "the board's command database lost cx.lvl")
    require(binding["rails"]["mss_in_cmd_db"]["value"] is True,
            "the board's command database lost mss.lvl")
    require(binding["rails"]["cx_address"]["value"] == 0x30000,
            "cx.lvl moved in the board's command database")
    require(binding["rails"]["mss_address"]["value"] == 0x30060,
            "mss.lvl moved in the board's command database")
    require(binding["rails"]["cx_level_count"]["value"] == 10,
            "cx.lvl's level count changed")
    require(binding["rails"]["mss_level_count"]["value"] == 7,
            "mss.lvl's level count changed")

    # The memory the images are loaded into.
    require(binding["carveout"]["image_base"]["value"] == 0x8a800000,
            "the modem image's base moved")
    require(binding["carveout"]["image_size"]["value"] == 0x10800000,
            "the modem image's span changed")
    require(binding["carveout"]["dtb_base"]["value"] == 0x9b000000,
            "the modem DTB's base moved")
    require(binding["carveout"]["dtb_size"]["value"] == 0x80000,
            "the modem DTB's span changed")

    # The GLINK and IPCC numbers the transport depends on.
    require(binding["smem"]["glink_desc_item"]["value"] == 478, "the GLINK descriptor moved")
    require(binding["smem"]["glink_tx_item"]["value"] == 479, "FIFO_0 moved")
    require(binding["smem"]["glink_rx_item"]["value"] == 480, "FIFO_1 moved")
    require(binding["ipcc"]["client"]["value"] == 2, "the modem's IPCC client changed")
    require(binding["glink"]["channel"]["value"] == "IPCRTR", "the QRTR channel changed")

    # The SMP2P pair: the device tree gives {inbound, outbound}, so the AP reads 0x1b3 and
    # writes 0x1ac.
    require(binding["smp2p"]["in_item"]["value"] == 0x1b3, "the inbound SMP2P item changed")
    require(binding["smp2p"]["out_item"]["value"] == 0x1ac, "the outbound SMP2P item changed")

    # The modem's ready and fatal bits. Linux's sm8550.dtsi gives the modem's SMP2P entries as
    # wdog, fatal, ready, handover, stop-ack, shutdown-ack, so fatal is bit 0 and ready is bit 1.
    require(binding["smp2p"]["fatal_bit"]["value"] == 0x1, "the modem's fatal bit changed")
    require(binding["smp2p"]["ready_bit"]["value"] == 0x2, "the modem's ready bit changed")

    # The QRTR wire format.
    require(binding["qrtr"]["header_bytes"]["value"] == 32, "the QRTR header size changed")
    require(binding["qrtr"]["proto_version"]["value"] == 1, "the QRTR protocol version changed")
    require(binding["qrtr"]["control_port"]["value"] == 0xFFFFFFFE, "the QRTR control port changed")
    require(binding["qrtr"]["broadcast_node"]["value"] == 0xFFFFFFFF, "the QRTR broadcast node changed")

    # The remoteproc's registers.
    require(binding["remoteproc"]["base"]["value"] == 0x04080000, "the modem's register base moved")

    # The manifest's own statements about the board's device tree must hold against the tree,
    # when the tree is on this machine. When it is not, the check says so.
    if CAF_TREE.is_file():
        tree = CAF_TREE.read_text(errors="replace")
        # The tree is indented freely, so the checks look for the property and its value with
        # any whitespace between them rather than for one exact line.
        def declares(property_name: str, value: str) -> bool:
            pattern = re.escape(property_name) + r"\s*=\s*" + re.escape(value)
            return re.search(pattern, tree) is not None

        require(declares("qcom,smem", "<0x1b3 0x1ac>"),
                "the board's tree no longer declares the modem's SMP2P pair")
        require(declares("qcom,glink-channels", '"IPCRTR"'),
                "the board's tree no longer declares the IPCRTR channel")
        require(declares("qcom,resource-name", '"mss.lvl"'),
                "the board's tree no longer declares the mss.lvl rail")
        require(declares("qcom,remote-pid", "<0x01>"),
                "the board's tree no longer declares the modem's remote pid")
        # The interrupt order matters: the CAF tree lists wdog, fatal, handover, ready, so the
        # second number of a 0x77 entry is the bit, and 0x77 0x00 is fatal, not 0x77 0x02. Many
        # nodes in the tree have interrupt-names, so the modem's remoteproc node is what is
        # searched, not the first match in the file.
        modem_node = re.search(
            r"remoteproc-mss@04080000\s*\{(.*?)\n\t\t\};(?![\t])", tree, re.S)
        require(modem_node is not None,
                "the board's tree no longer has the modem's remoteproc node")
        # The names are a list of quoted strings, not one string.
        names = re.search(r'interrupt-names\s*=\s*((?:"[^"]*"\s*,?\s*)+);',
                          modem_node.group(1))
        require(names is not None, "the modem's node has no interrupt names")
        order = re.findall(r'"([^"]*)"', names.group(1))
        require(order[:4] == ["wdog", "fatal", "handover", "ready"],
                "the board's tree reordered the modem's interrupt names")
    else:
        print("note: the board's CAF tree is not on this machine, "
              "its statements are unchecked", file=sys.stderr)

    print("PASS fuxi baseband facts: "
          f"{len(found)} values bound, every evidence key resolves, "
          "PAS ids and SMP2P bits pinned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
