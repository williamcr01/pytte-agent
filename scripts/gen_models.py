#!/usr/bin/env python3
"""Regenerate src/ai/models.gen.inc, the built-in model catalog.

Sources, like pi's generator:
  anthropic, openai   https://models.dev/api.json
  openrouter          https://openrouter.ai/api/v1/models

Only models that can call tools are kept. The output is committed, so builds
stay offline; run `make models` to refresh it and review the diff.

Model behavior that no source records (compat flags) lives in COMPAT below.

The table grows with each provider, which is fine. Later (M5): a build option
to compile in only some providers, for small targets.
"""
import json
import re
import sys
import urllib.request
from decimal import Decimal

OUT = "src/ai/models.gen.inc"

# provider/model -> pt_compat initializer, for what the sources do not say.
COMPAT = {
    "anthropic/claude-opus-5-5": ".mid_conversation_system = PT_YES, .fallbacks = PT_YES",
    "anthropic/claude-opus-5": ".mid_conversation_system = PT_YES, .fallbacks = PT_YES",
    "anthropic/claude-fable-5-1": ".mid_conversation_system = PT_YES",
    "anthropic/claude-sonnet-5-5": ".mid_conversation_system = PT_YES",
    "anthropic/claude-opus-4-8": ".mid_conversation_system = PT_YES",
}



def reasoning(provider, mid, listed):
    # pytte sends adaptive thinking, which starts with Claude 4.6; older
    # Claude models only take thinking budgets, which pytte does not send.
    if provider == "anthropic" and re.match(r"claude-[a-z]+-(3|4-[0-5])($|[-.])", mid):
        return False
    return listed


# Effort names -> PT_EFF_* (pytte's levels, "none" is off). Like pi's
# getEffortThinkingLevelMap: a level counts only when the source lists it.
EFFORT = {"none": "PT_EFF_OFF", "minimal": "PT_EFF_MINIMAL", "low": "PT_EFF_LOW",
          "medium": "PT_EFF_MEDIUM", "high": "PT_EFF_HIGH", "xhigh": "PT_EFF_XHIGH", "max": "PT_EFF_MAX"}


def efforts_dev(options):
    """models.dev reasoning_options -> the levels; toggle also allows off.
    A model listing no effort values is unknown ([]): its API decides."""
    values = [v for o in options or [] if o.get("type") == "effort" for v in o.get("values") or []]
    levels = {v for v in values if v in EFFORT}
    if levels <= {"none"}:  # nothing but off (or "default"): unknown
        return []
    if any(o.get("type") == "toggle" for o in options):
        levels.add("none")
    return sorted(levels, key=list(EFFORT).index)


def efforts_openrouter(reasoning):
    """OpenRouter's reasoning metadata -> the levels; off unless mandatory.
    Without supported_efforts OpenRouter maps the level itself: low, medium
    and high, which every reasoning model takes."""
    if not reasoning:
        return []
    levels = {v for v in reasoning.get("supported_efforts") or [] if v in EFFORT}
    if not levels:
        if reasoning.get("mandatory"):
            return []
        levels = {"low", "medium", "high"}
    if not reasoning.get("mandatory"):
        levels.add("none")
    return sorted(levels, key=list(EFFORT).index)


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "pytte gen_models"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def micro(dollars_per_million):
    """$ per million tokens -> micro-dollars per million tokens."""
    v = Decimal(str(dollars_per_million or 0)) * 1000000
    return max(0, int(v.to_integral_value()))


def from_models_dev(data, provider):
    rows = []
    for mid, m in data[provider]["models"].items():
        if not m.get("tool_call") or m.get("status") == "deprecated" or "realtime" in mid:
            continue
        limit, cost = m.get("limit") or {}, m.get("cost") or {}
        rows.append({
            "provider": provider, "id": mid, "name": m.get("name") or mid,
            "context": limit.get("context") or 0, "output": limit.get("output") or 0,
            "reasoning": reasoning(provider, mid, bool(m.get("reasoning"))),
            "images": "image" in (m.get("modalities") or {}).get("input", []),
            "cost": [micro(cost.get(k)) for k in ("input", "output", "cache_read", "cache_write")],
            "efforts": efforts_dev(m.get("reasoning_options")),
        })
    return rows


def from_openrouter(data):
    rows = []
    for m in data["data"]:
        params = m.get("supported_parameters") or []
        pricing = m.get("pricing") or {}
        if "tools" not in params or Decimal(pricing.get("prompt") or "0") < 0:
            continue
        per_token = lambda k: (Decimal(pricing.get(k) or "0") * 1000000)  # noqa: E731
        rows.append({
            "provider": "openrouter", "id": m["id"], "name": m.get("name") or m["id"],
            "context": m.get("context_length") or 0,
            "output": (m.get("top_provider") or {}).get("max_completion_tokens") or 0,
            "reasoning": "reasoning" in params,
            "images": "image" in (m.get("architecture") or {}).get("input_modalities", []),
            "cost": [micro(per_token(k)) for k in
                     ("prompt", "completion", "input_cache_read", "input_cache_write")],
            "efforts": efforts_openrouter(m.get("reasoning")),
        })
    return rows


def c_str(s):
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if c in '"\\':
            out.append("\\" + c)
        elif 32 <= b < 127 and c != "?":  # "?" avoids trigraphs
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


# Index macros into providers[] in src/ai/providers.c, and the string caps
# of pt_model_entry in include/pytte_ai.h.
PROVIDER = {"anthropic": "PROV_ANTHROPIC", "openai": "PROV_OPENAI", "openrouter": "PROV_OPENROUTER"}
ID_CAP, NAME_CAP = 52, 48


def row(r):
    ref = r["provider"] + "/" + r["id"]
    for k, cap in (("id", ID_CAP), ("name", NAME_CAP)):
        if len(r[k].encode("utf-8")) >= cap:
            sys.exit("gen_models: %s %r does not fit pt_model_entry (cap %d)" % (k, r[k], cap))
    return "{%s, %s, %s, %s, %s, %d, %d,\n {%s},\n {%s},\n %s},\n" % (
        PROVIDER[r["provider"]], c_str(r["id"]), c_str(r["name"]),
        "true" if r["reasoning"] else "false", "true" if r["images"] else "false",
        r["context"], r["output"], ", ".join(str(c) for c in r["cost"]), COMPAT.get(ref, "0"),
        " | ".join(EFFORT[e] for e in r["efforts"]) or "0")


def main():
    dev = fetch("https://models.dev/api.json")
    rows = from_models_dev(dev, "anthropic") + from_models_dev(dev, "openai")
    rows += from_openrouter(fetch("https://openrouter.ai/api/v1/models"))
    rows.sort(key=lambda r: (["anthropic", "openai", "openrouter"].index(r["provider"]), r["id"]))
    missing = set(COMPAT) - {r["provider"] + "/" + r["id"] for r in rows}
    if missing:
        sys.exit("gen_models: COMPAT names models the sources no longer list: %s" % sorted(missing))
    with open(OUT, "w") as f:
        f.write("/* Generated by scripts/gen_models.py (make models); do not edit.\n"
                " * provider, id, name, reasoning, images, context window, max output,\n"
                " * cost (micro-dollars per million tokens: input, output, cache read,\n"
                " * cache write), compat, efforts (0: unknown). */\n")
        f.writelines(row(r) for r in rows)
    counts = {p: sum(r["provider"] == p for r in rows) for p in ("anthropic", "openai", "openrouter")}
    print("gen_models: wrote %s: %s" % (OUT, counts))


if __name__ == "__main__":
    main()
