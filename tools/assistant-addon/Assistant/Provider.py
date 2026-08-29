"""Assistant - OpenAI-compatible chat provider (streaming SSE via requests).

Works with DeepSeek, OpenAI, Ollama (`/v1/chat/completions`), LM Studio, vLLM ...
`stream_chat` yields dict chunks:

    {"type":"text_delta","delta": str}
    {"type":"tool_calls","calls": [ {name, arguments(dict)} ]}
    {"type":"done","content": str, "finish_reason": str|None}
    {"type":"error","error": str}
"""

import json
import os
from typing import Dict, List, Optional

import requests


class ProviderError(RuntimeError):
    pass


def _endpoint(path="/chat/completions"):
    import Preferences as P
    ep = (P.endpoint() or "https://api.deepseek.com").rstrip("/")
    if ep.endswith("/v1"):
        return ep + path
    if ep.endswith("/chat/completions"):
        return ep
    return ep + path


def _looks_like_key(k):
    """A usable secret token: a single, shortish alphanumeric-ish token."""
    return bool(k) and 8 <= len(k) <= 400 and all(
        c.isalnum() or c in "-_." for c in k)


def _headers():
    import Preferences as P
    key = P.api_key()
    h = {"Content-Type": "application/json"}
    if key and _looks_like_key(key):
        h["Authorization"] = "Bearer " + key
    return h


def _payload(messages, model=None, tools=None, temperature=None, stream=True,
             max_tokens=None, image=None):
    import Preferences as P
    model = model or P.model()
    body = {
        "model": model,
        "messages": messages,
        "stream": stream,
        "temperature": P.temperature() if temperature is None else temperature,
    }
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    if max_tokens:
        body["max_tokens"] = max_tokens
    return body


def stream_chat(messages: List[Dict], tools=None, model=None,
                temperature=None, max_tokens=None, timeout=None,
                mock=False, mock_script=None):
    """Stream a chat completion.  `messages` is a full OpenAI message list.

    When `mock` is True (tests / offline) a scripted provider is used instead of
    real HTTP (no network).  `mock_script` is a list of turn-dicts. Inherits the
    same chunk protocol so the Agent loop is provider-agnostic.
    """
    import Preferences as P
    if mock or os.environ.get("ASSISTANT_MOCK"):
        yield from _mock_stream(mock_script, temperature)
        return

    # Guard: a missing/invalid key would otherwise become a cryptic
    # requests InvalidHeader or a raw 401 - surface a clear message instead.
    if not _looks_like_key(P.api_key()):
        yield {"type": "error",
               "error": ("No API key configured. Set the DEEPSEEK_API_KEY env "
                         "var, or open Assistant -> Settings and paste your "
                         "DeepSeek API key.")}
        return

    timeout = timeout or P.timeout()
    url = _endpoint()
    body = _payload(messages, model=model, tools=tools,
                    temperature=temperature, max_tokens=max_tokens)

    text = ""
    reasoning = ""
    pending_tool_calls = {}
    finish_reason = None
    usage = None

    def _flush_tool_calls():
        if not pending_tool_calls:
            return
        calls = []
        for idx in sorted(pending_tool_calls):
            tc = pending_tool_calls[idx]
            args = tc.get("arguments", "")
            try:
                arg = json.loads(args) if args else {}
            except Exception:
                arg = {"_raw": args}
            calls.append({"name": tc.get("name", ""), "arguments": arg})
        yield {"type": "tool_calls", "calls": calls}

    req = requests.post(url, headers=_headers(), json=body, stream=True,
                        timeout=timeout, proxies=None)
    if req.status_code >= 400:
        detail = req.text[:500]
        req.close()
        raise ProviderError(f"HTTP {req.status_code} from {url}: {detail}")

    try:
        for line in req.iter_lines(decode_unicode=True):
            if not line or not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if data == "[DONE]":
                break
            try:
                chunk = json.loads(data)
            except Exception:
                continue
            choice = chunk.get("choices", [{}])[0]
            delta = choice.get("delta", {})
            # Capture thinking-mode reasoning independently of content (deepseek
            # streams it across many deltas; an if/elif would drop the ones that
            # also carry content, and a truncated echo makes the API reject it).
            rc = delta.get("reasoning_content") or chunk.get("reasoning_content")
            if rc:
                reasoning += str(rc)
            content = delta.get("content")
            if content:
                text += content
                yield {"type": "text_delta", "delta": content}
            if delta.get("tool_calls"):
                for tc in delta["tool_calls"]:
                    idx = tc.get("index", 0)
                    pc = pending_tool_calls.setdefault(idx, {"name": "", "arguments": ""})
                    if tc.get("id"):
                        pc["id"] = tc["id"]
                    if tc.get("function"):
                        if tc["function"].get("name"):
                            pc["name"] = tc["function"]["name"]
                        if tc["function"].get("arguments"):
                            pc["arguments"] += tc["function"]["arguments"]
            if choice.get("finish_reason"):
                finish_reason = choice["finish_reason"]
            if chunk.get("usage"):
                usage = chunk["usage"]
        yield from _flush_tool_calls()
        yield {"type": "done", "content": text, "finish_reason": finish_reason,
               "reasoning_content": reasoning, "usage": usage}
    finally:
        req.close()


class MockSeq:
    """Stateful mock turn-sequence, so a multi-round tool loop can be scripted."""

    def __init__(self, turns):
        self.turns = turns
        self.i = 0

    def next(self):
        t = self.turns[min(self.i, len(self.turns) - 1)]
        self.i += 1
        return t


def _mock_stream(script, temperature):
    """Deterministic provider for tests.

    `script` is either a MockSeq or a list of turns; each turn is a chunk-dict OR a
    list of chunk-dicts.  Turns are consumed in order across successive stream_chat
    calls (so [tool_call_turn, text_turn] exercises a full multi-round tool loop).
    """
    turns = script or [
        {"type": "tool_calls", "calls": [{"name": "list_objects", "arguments": {}}]},
        [{"type": "text_delta", "delta": "Here is the scene."},
         {"type": "done", "content": "Here is the scene.", "finish_reason": "stop"}],
    ]
    t = turns.next() if isinstance(turns, MockSeq) else turns[0]
    to_yield = t if isinstance(t, (list, tuple)) else [t]
    for chunk in to_yield:
        yield chunk
