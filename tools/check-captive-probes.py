#!/usr/bin/env python3
"""Guard the captive-portal probe handlers against two invisible bug classes.

Both shipped once (2026-07-19) and neither is visible to code review:

  A. A DEAD HANDLER — probe_redirect() has a `strcmp(req->uri, "/x")` branch,
     but "/x" is never registered with httpd_register_uri_handler. ESP-IDF's
     httpd only dispatches REGISTERED uris; the branch is unreachable, and the
     path falls to not_found_handler (302 with no acked check → a RELEASED
     client bounced back to /welcome). The handler body is correct, so a review
     of probe_redirect sees nothing. This was the Firefox /success.txt bug.

  B. SPEC DRIFT — the set of probe paths, or a genuine-success body, diverges
     from CONTRACT.md § Captive onboarding (the single spec both hubs reconcile
     to). A missing route means that OS's captive sheet never dismisses; a wrong
     body means the OS reads a released client as still-captive. This was hubd
     missing the Firefox arm the ESP had.

The check reads the canonical PATH SET from hub's CONTRACT.md (like
sync-dashboard.sh reads canonical dashboard.html), and holds the exact
genuine-success BYTES here — a markdown cell can't unambiguously encode
"success\\n". Doc, this table, and the impl are all asserted to agree, so no
two can drift without a red.

Run:  tools/check-captive-probes.py        (sibling ../../hub, or $HUB_REPO)
CI:   HUB_REPO=$PWD/.hub tools/check-captive-probes.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORTAL = os.path.join(HERE, "..", "src", "wifi_portal.c")
HUB = os.environ.get("HUB_REPO", os.path.join(HERE, "..", "..", "hub"))
CONTRACT = os.path.join(HUB, "CONTRACT.md")

# The genuine-success bytes each probe path must return to a GREETED client.
# None = a bare status with no body (Android's 204). Keyed by path; the key set
# is the canonical probe set and is asserted equal to CONTRACT.md's table.
EXPECTED = {
    "/generate_204":       None,
    "/hotspot-detect.html": "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>",
    "/connecttest.txt":    "Microsoft Connect Test",
    "/ncsi.txt":           "Microsoft NCSI",
    "/success.txt":        "success\\n",   # exact: lowercase, trailing newline
}


def die(bug, msg):
    print(f"captive-probes: FAIL [{bug}] {msg}", file=sys.stderr)
    sys.exit(1)


def read(path, what):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        print(f"captive-probes: cannot read {what}: {e}", file=sys.stderr)
        print(f"  clone sprocket-robotics/hub as a sibling, or set HUB_REPO=path/to/hub",
              file=sys.stderr)
        sys.exit(2)


def contract_paths(text):
    """Paths in the § Captive onboarding genuine-success table (first `code` cell
    of each data row). The table ends at the first blank line after it."""
    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines)
                     if l.startswith("## Captive onboarding"))
    except StopIteration:
        die("B", "CONTRACT.md has no '## Captive onboarding' section")
    paths, in_table = set(), False
    for l in lines[start:]:
        if l.startswith("| Probe path"):
            in_table = True
            continue
        if in_table:
            if not l.startswith("|"):
                break
            if set(l) <= set("| -"):     # the |---|---| separator row
                continue
            cell = l.split("|")[1].strip()
            m = re.match(r"`(/[^`]+)`", cell)
            if m:
                paths.add(m.group(1))
    return paths


def portal_facts(text):
    """(registered→probe_redirect paths, paths strcmp'd inside probe_redirect)."""
    # var -> (uri, handler) from `httpd_uri_t u_x = { .uri = "..", .handler = h }`
    decls = {}
    for m in re.finditer(
            r"httpd_uri_t\s+(\w+)\s*=\s*\{[^}]*?\.uri\s*=\s*\"([^\"]+)\"[^}]*?"
            r"\.handler\s*=\s*(\w+)", text, re.S):
        decls[m.group(1)] = (m.group(2), m.group(3))
    registered_vars = set(re.findall(r"httpd_register_uri_handler\([^,]+,\s*&(\w+)\)", text))
    registered_probe = {decls[v][0] for v in registered_vars
                        if v in decls and decls[v][1] == "probe_redirect"}

    body = re.search(r"static esp_err_t probe_redirect\(.*?\n\}\n", text, re.S)
    if not body:
        die("A", "could not locate probe_redirect() in wifi_portal.c")
    strcmp_paths = set(re.findall(r'strcmp\(req->uri,\s*"([^"]+)"\)', body.group(0)))
    return registered_probe, strcmp_paths


def main():
    contract = read(CONTRACT, "hub CONTRACT.md")
    portal = read(PORTAL, "src/wifi_portal.c")

    canon = set(EXPECTED)
    doc = contract_paths(contract)
    registered, strcmp_paths = portal_facts(portal)

    # This table and the doc must agree on the path set (neither can gain/lose a
    # row silently), and the doc must carry every exact body (so a body edit in
    # one place reds until the other follows).
    if canon != doc:
        die("B", f"CONTRACT.md path set {sorted(doc)} != this check's {sorted(canon)} "
                 f"— update whichever is stale (they are the same spec)")
    # Exact, not substring: `Microsoft NCSI` is a substring of a botched
    # `Microsoft NCSII`. The doc writes each body in a `backtick` span; the impl
    # as a "quoted" C literal.
    for path, want in EXPECTED.items():
        if want and f"`{want}`" not in contract:
            die("B", f"CONTRACT.md is missing the genuine-success body for {path}: {want!r}")

    # Bug B: the ESP must register EXACTLY the spec's probe paths — no missing
    # route (that OS never dismisses) and no unlisted extra.
    if registered != canon:
        missing, extra = canon - registered, registered - canon
        parts = []
        if missing:
            parts.append(f"NOT registered → probe_redirect (released clients bounce): {sorted(missing)}")
        if extra:
            parts.append(f"registered but not in the spec: {sorted(extra)}")
        die("B", "; ".join(parts))

    # Bug A: every path probe_redirect distinguishes by strcmp must be reachable
    # — i.e. registered. An unregistered strcmp branch is dead code.
    dead = strcmp_paths - registered
    if dead:
        die("A", f"probe_redirect handles {sorted(dead)} but they are never registered "
                 f"— dead code; add httpd_register_uri_handler + bump max_uri_handlers")

    # Bug B: the impl must serve each exact body (as a "quoted" C literal, so a
    # botched "Microsoft NCSII" no longer matches "Microsoft NCSI").
    for path, want in EXPECTED.items():
        if want and f'"{want}"' not in portal:
            die("B", f"wifi_portal.c does not serve the genuine-success body for {path}: {want!r}")

    print(f"captive-probes: OK — {len(canon)} probe paths registered, handled, "
          f"and byte-matched to CONTRACT.md § Captive onboarding")


if __name__ == "__main__":
    main()
