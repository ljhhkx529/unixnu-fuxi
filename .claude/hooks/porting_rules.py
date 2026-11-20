#!/usr/bin/env python3
"""The porting rules as a hook: compile before polishing, look parameters up in the map
instead of in the upstream source, and fix a defect where it is found.

The hook reads the tool input on stdin and decides whether the call is allowed to
proceed. It never rewrites the call: it either passes it through, or it blocks it with
a reason the agent can act on.

Four rules, each with its own check:

1. A write of a source file under package/ or the component's platform/ tree must be
   followed by a compile of that file. The hook records the writes and blocks a *read*
   of upstream source for a parameter the map already answers.
2. A Read of an upstream Linux source file is blocked when PORTING_MAP.md already cites
   that file as the origin of a value it carries, unless the map's "待查的上游源码"
   section lists the file: a file listed there is a declared work item, and reading it
   is how the answer is found. A file the map cites nowhere is new ground and is
   allowed, but the answer belongs in the map afterwards.
3. A Grep over the upstream kernel tree for a parameter that the map already lists is
   blocked.
4. A defect named in the agent's own notes must be fixed at once, not deferred: the
   notes are the source of truth, so a defect listed under PORTING_BLOCKERS.md's
   "已发现的缺陷" heading blocks the next write of an unrelated file until the
   defect's file has been written. The other entries of that document are open
   questions waiting on a device measurement and do not block anything.
5. A bug that is not understood must not be theorised about twice. When the agent has
   already changed a file for a bug whose cause it could not name, the next write to
   that same file has to carry instrumentation — a print of the state the hypothesis
   turns on — or the write is refused. The rule is what keeps a round from becoming a
   sequence of guesses: one hypothesis, one test, and the measurement that decides it.

The state lives in .claude/hooks/state.json so it survives between calls.
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = Path(__file__).resolve().parent / "state.json"
MAP = ROOT / "PORTING_MAP.md"
BLOCKERS = ROOT / "PORTING_BLOCKERS.md"
MANIFEST = ROOT / "facts/baseband/manifest.toml"

# The test points these at scratch copies, so a test run never reads the real notes and never
# disturbs the real state.
STATE = Path(os.environ.get("PORTING_HOOK_STATE", STATE))
BLOCKERS = Path(os.environ.get("PORTING_HOOK_BLOCKERS", BLOCKERS))
MAP = Path(os.environ.get("PORTING_HOOK_MAP", MAP))

# The upstream kernel trees this repository reads from. A read inside one of them is
# a source lookup, not a read of our own code.
#
# These are the trees that are actually in use: the checkout under /tmp/x13-dbg, the tarball
# unpacked at /tmp/lx, the work tree at /tmp/x13/work (which holds the kernel source archive
# and the board's own device tree), and the reference tree of another port.
UPSTREAM_ROOTS = tuple(
    part for part in os.environ.get(
        "PORTING_HOOK_UPSTREAM_ROOTS",
        "/tmp/x13-dbg/tree/linux-:/tmp/lx/:/tmp/x13/work/:/Users/mac/Documents/Codex/"
        "2026-11-08/cong/work/baseband-references/",
    ).split(":") if part)

# The sources the map names as the first-hand origin of a value. Reading these is
# always allowed: they are where the evidence comes from.
ALWAYS_ALLOWED = (
    "PORTING_MAP.md",
    "PORTING_STATUS.md",
    "PORTING_BLOCKERS.md",
)

# The heading under which the map lists the upstream files whose answer is still missing.
# A file listed there is a declared work item: reading it is allowed, and the answer
# belongs in the map afterwards.
PENDING_HEADING = "待查的上游源码"

# A source file name, with or without a directory part.
SOURCE_TOKEN = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:c|h|dtsi?|yaml|mk)\b")

# The table cells that name our own code rather than an upstream source.
OWN_CODE_MARKERS = ("本仓", "UniXNU", "unixnu-fuxi", "自研")


def load_state():
    if STATE.is_file():
        try:
            state = json.loads(STATE.read_text())
        except ValueError:
            state = {}
    else:
        state = {}
    state.setdefault("dirty", [])
    # suspect maps a file to the number of times it was written for a bug whose cause was
    # not named. See rule 5.
    state.setdefault("suspect", {})
    # A bug under investigation is declared in the blockers document, so the declaration is
    # in the notes and not only in the agent's head. A file whose entry now states its cause
    # is no longer under investigation, so it is dropped from the state.
    chasing = under_investigation()
    for candidate in list(state["suspect"]):
        if candidate not in chasing:
            del state["suspect"][candidate]
    for candidate in chasing:
        state["suspect"][candidate] = max(int(state["suspect"].get(candidate, 0)), 1)
    # defects maps a heading to the file that fixes it. An older state may have keyed this by
    # path, which is the same information the other way round.
    defects = state.get("defects", {})
    if isinstance(defects, dict):
        state["defects"] = {f"defect in {path}": path for path in defects}
    else:
        state["defects"] = {}
    return state


def save_state(state):
    STATE.write_text(json.dumps(state, indent=2) + "\n")


def map_text():
    if not MAP.is_file():
        return ""
    return MAP.read_text(errors="replace")


def map_values():
    """The parameter names the map already answers, as lowercase words."""
    words = set()
    for line in map_text().lower().splitlines():
        if not line.strip().startswith("|"):
            continue
        for cell in line.split("|"):
            cell = cell.strip().strip("`").strip()
            if not cell or cell in ("---",):
                continue
            # A value cell: an identifier, a number, or a hex constant.
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*|0x[0-9a-f]+|\d+", cell):
                words.add(cell.lower())
    return words


def is_own_code(token: str) -> bool:
    """Whether a source token names a file of this repository rather than an upstream one."""
    for prefix in ("unixnu-fuxi/",):
        if token.startswith(prefix):
            token = token[len(prefix):]
    return (ROOT / token).is_file()


def source_tokens(cell: str) -> list:
    """The upstream source files one table cell names.

    A cell that says the answer comes from this repository's own code names no upstream
    file, however many paths it holds.
    """
    if any(marker in cell for marker in OWN_CODE_MARKERS):
        return []
    tokens = []
    for token in SOURCE_TOKEN.findall(cell):
        token = token.rstrip(".")
        if is_own_code(token):
            continue
        if token not in tokens:
            tokens.append(token)
    return tokens


def cited_sources() -> dict:
    """The upstream files the map cites, mapped to the values it cites them for.

    Every table row in the map is read as a subject and a source: the first cell is the
    subject, the rest of the row may name the source.
    """
    cited = {}
    for line in map_text().splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if len(cells) < 2 or set(cells) <= {"---"}:
            continue
        subject = re.sub(r"[*`\s]", "", cells[0]).lower()
        for cell in cells[1:]:
            for token in source_tokens(cell):
                cited.setdefault(token, set()).add(subject)
    return cited


def pending_sources() -> set:
    """The upstream files the map's "待查的上游源码" section lists."""
    text = map_text()
    heading = re.search(rf"^##\s+{PENDING_HEADING}\s*$", text, re.M)
    if heading is None:
        return set()
    rest = text[heading.end():]
    end = re.search(r"^##\s(?!#)", rest, re.M)
    body = rest[:end.start()] if end else rest
    pending = set()
    for cell in body.splitlines():
        pending.update(source_tokens(cell))
    return pending


def board_first_hand() -> set:
    """The paths the manifest names as the board's own first-hand source.

    Reading the board's own device tree is where the evidence comes from, so it is allowed
    even though the path is outside this repository.
    """
    if not MANIFEST.is_file():
        return set()
    try:
        import tomllib
    except ImportError:
        return set()
    try:
        with MANIFEST.open("rb") as stream:
            manifest = tomllib.load(stream)
    except (ValueError, OSError):
        return set()
    allowed = set()
    for name, section in manifest.items():
        if not name.startswith("source.") or not isinstance(section, dict):
            continue
        if "board-first-hand" not in str(section.get("kind", "")):
            continue
        path = section.get("path")
        if isinstance(path, str) and path.startswith("/"):
            allowed.add(path)
    return allowed


def is_upstream(path):
    return any(path.startswith(root) for root in UPSTREAM_ROOTS)


def is_ours(path):
    return str(path).startswith(str(ROOT))


def matches_source(read_path: str, cited: str) -> bool:
    """Whether a path being read is the source file a map entry cites."""
    if "/" in cited:
        return read_path.endswith("/" + cited)
    return Path(read_path).name == cited


# A defect is recorded in PORTING_BLOCKERS.md, under the "已发现的缺陷" heading. Only the
# entries under it are defects a write can fix; the other entries of that document are open
# questions waiting on a device measurement and must not block anything.


def noted_defects():
    """The defects the notes carry, as {heading: the file that fixes it}.

    Only the "已发现的缺陷" section counts. The other entries of the blockers document are
    open questions waiting on a device measurement, not code defects a write can fix, and
    they must not block the work.
    """
    if not BLOCKERS.is_file():
        return {}
    text = BLOCKERS.read_text(errors="replace")
    # The heading is matched at the start of a line, so the copy of it inside the section's
    # own comment does not match first.
    heading_match = re.search(r"^##\s+已发现的缺陷\s*$", text, re.M)
    if heading_match is None:
        return {}
    rest = text[heading_match.end():]
    # The section ends at the next top-level heading, which is one that is not a sub-heading.
    end = re.search(r"^##\s(?!#)", rest, re.M)
    body = rest[:end.start()] if end else rest

    found = {}
    for section in re.split(r"^###\s+", body, flags=re.M)[1:]:
        heading = section.splitlines()[0].strip()
        # The file to fix is the first repository-relative path the entry names.
        for match in re.finditer(r"`([A-Za-z0-9_./-]+\.(?:c|cpp|h|py|rs|toml|patch))`",
                                 section):
            candidate = match.group(1)
            if (ROOT / candidate).is_file():
                found[heading] = candidate
                break
    return found


def under_investigation():
    """The files a bug under investigation is being chased in, as a set of paths.

    A bug whose cause is not established is declared in the blockers document's
    "待查的缺陷" section: the entry names the file the chase is in. The hook then requires
    the next write of that file to carry a print, so a second guess becomes a measurement.

    An entry that states its cause is no longer a chase: the cause is written down, so the
    file is free again and the fix can go in.
    """
    if not BLOCKERS.is_file():
        return set()
    text = BLOCKERS.read_text(errors="replace")
    heading_match = re.search(r"^##\s+待查的缺陷\s*$", text, re.M)
    if heading_match is None:
        return set()
    rest = text[heading_match.end():]
    end = re.search(r"^##\s(?!#)", rest, re.M)
    body = rest[:end.start()] if end else rest
    found = set()
    for section in re.split(r"^###\s+", body, flags=re.M)[1:]:
        # The cause is stated in the entry, so the chase is over.
        if re.search(r"^\*\*原因\*\*", section, re.M):
            continue
        for match in re.finditer(r"`([A-Za-z0-9_./-]+\.(?:c|cpp|h|py|rs|toml|patch))`", section):
            candidate = match.group(1)
            if (ROOT / candidate).is_file():
                found.add(candidate)
    return found


def blocking_reason(state, path):
    """Why this write has to wait, or None if it may go ahead.

    Rule 4: a defect that was found and named must be fixed at once. While a noted defect's
    file has not been written, a write to any other file is refused, so the defect cannot be
    quietly left behind while the work moves on.
    """
    defects = state.get("defects", {})
    if not defects:
        return None
    target = str(path)
    for heading, file_path in defects.items():
        if file_path and file_path in target:
            # This write is the fix itself: it clears the defect.
            return None
        if not file_path:
            continue
        if target.startswith(str(ROOT)) and target != file_path:
            return (
                f"Write of {Path(target).name} is blocked: a defect is waiting to be fixed and "
                f"must not be left behind. Fix it first: {heading} ({file_path}). "
                "Write that file, then continue."
            )
    return None


def added_measurement(path, params):
    """Whether this write adds a print, and only a print that was not there before.

    A whole-file rewrite carries every print the file already had, so counting them would let
    the rule be passed by rewriting the file as it was. What counts is a print that is new:
    for an edit that is the text the edit adds, for a rewrite it is the prints the old file
    did not have.
    """
    new_text = params.get("new_string") or ""
    if not new_text:
        new_text = params.get("content") or ""
        if not new_text:
            for edit in params.get("edits") or []:
                new_text += edit.get("new_string") or ""
        old_text = ""
        try:
            old_text = Path(str(path)).read_text(errors="replace")
        except OSError:
            old_text = ""
        if old_text:
            new_prints = set(re.findall(r"printf\s*\(\s*\"([^\"]*)\"", new_text))
            old_prints = set(re.findall(r"printf\s*\(\s*\"([^\"]*)\"", old_text))
            added = new_prints - old_prints
            return bool(added)
    return bool(re.search(r"printf\s*\(", new_text))


def speculation_reason(state, path, params):
    """Why this write has to carry instrumentation, or None if it may go ahead.

    Rule 5: a bug whose cause is not known must not be theorised about twice. The hook cannot
    tell whether a write is a guess, so it uses what it can measure: a file that is under
    investigation for a bug, written again with no new print, is the shape of a second guess.
    The write is then refused until the file carries a print of the state the hypothesis turns
    on.

    The counter is per file and it is cleared by a write that adds a print, so a file that has
    been instrumented is free to be written again.
    """
    suspect = state.get("suspect", {})
    if not suspect:
        return None
    target = str(path)
    for suspect_path, count in suspect.items():
        if suspect_path not in target:
            continue
        if count < 1:
            continue
        if added_measurement(path, params):
            # The write prints something new: it measures. The counter goes back to zero, so
            # the next write of this file is allowed again.
            suspect[suspect_path] = 0
            continue
        suspect[suspect_path] = count + 1
        return (
            f"Write of {Path(target).name} is blocked: this file is under investigation for a "
            "bug whose cause is not established, and this write adds no measurement. Add a "
            "print of the state the hypothesis turns on (the register, the stage, the error), "
            "so the next round has data instead of a second guess. Write the file with the "
            "print, then continue."
        )
    state["suspect"] = suspect
    return None


def upstream_read_reason(path):
    """Why reading this upstream file is refused, or None if it may be read.

    Rule 2: the map is the only entry point for a parameter. A file the map already cites
    as the origin of a value it carries has been mined; reading it again answers nothing
    the map does not already say. A file the map's pending list names is a declared work
    item, and a file the map cites nowhere is new ground: both are allowed.
    """
    if any(allowed in path for allowed in ALWAYS_ALLOWED):
        return None
    if path in board_first_hand():
        return None
    if not is_upstream(path):
        return None
    if any(matches_source(path, cited) for cited in pending_sources()):
        return None
    for cited, subjects in cited_sources().items():
        if not matches_source(path, cited):
            continue
        names = ", ".join(sorted(s for s in subjects if s)) or "the values it carries"
        return (
            f"Read of the upstream source is blocked: PORTING_MAP.md already answers "
            f"{names} from {cited}. Use the map. If the map's answer is not enough, list the "
            f"file under the map's \"{PENDING_HEADING}\" section first, read it, then write the "
            f"answer back into the map."
        )
    return None


def upstream_search_reason(command: str, pattern: str, path: str = ""):
    """Why searching the upstream tree is refused, or None if it may be searched.

    Rule 3: a search for a parameter the map already carries is the same repeated lookup
    rule 2 refuses, in the shape of a search rather than a read.
    """
    if not any(root in command or root in path for root in UPSTREAM_ROOTS):
        return None
    known = map_values()
    # The file names the map cites are known too: searching for one of them is the
    # repeated lookup the rule refuses.
    for cited in cited_sources():
        known.add(cited.lower())
        known.add(Path(cited).name.lower())
    for word in re.split(r"[|()\s]+", pattern):
        w = word.strip().strip("'\"").lower()
        if len(w) > 3 and w in known:
            return (
                f"Searching the upstream source is blocked: PORTING_MAP.md already answers "
                f"'{w}'. Use the map. Only search upstream when the value is an inference and "
                "the build is blocked on it."
            )
    return None


def main():
    payload = json.loads(sys.stdin.read() or "{}")
    tool = payload.get("tool_name", "")
    params = payload.get("tool_input", {}) or {}
    state = load_state()
    decision = None

    if tool in ("Write", "Edit", "MultiEdit", "NotebookEdit"):
        path = params.get("file_path", "")
        if is_ours(path) and path.endswith((".c", ".cpp", ".h")):
            state["dirty"].append(path)
        # The defects are read from the document every time, so a stale state cannot wedge the
        # work and a defect taken out of the document stops blocking at once.
        state["defects"] = noted_defects()
        # Rule 4: a defect the notes carry must be fixed before the work moves on.
        reason = blocking_reason(state, path)
        if reason:
            decision = reason
        else:
            # A write to the file that fixes a defect clears it: the defect is being fixed.
            for heading in [h for h, f in state["defects"].items()
                            if f and f in str(path)]:
                state["defects"].pop(heading, None)
            # Rule 5: a second guess about the same bug has to become a measurement.
            reason = speculation_reason(state, path, params)
            if reason:
                decision = reason
            else:
                save_state(state)

    elif tool in ("Read",):
        path = params.get("file_path", "")
        decision = upstream_read_reason(path)

    elif tool in ("Grep", "Bash"):
        pattern = params.get("pattern", "") or ""
        command = params.get("command", "") or ""
        decision = upstream_search_reason(command, pattern, params.get("path", "") or "")

    if decision:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                                 "permissionDecision": "deny",
                                                 "permissionDecisionReason": decision}}))
        return 0
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                             "permissionDecision": "allow"}}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
