#!/usr/bin/env python3
"""Small CLI client for Qt's public documentation MCP service."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any


ENDPOINT = "https://qt-docs-mcp.qt.io/mcp"
PROTOCOL_VERSION = "2025-11-25"


def request(method: str, params: dict[str, Any]) -> dict[str, Any]:
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    ).encode()
    http_request = urllib.request.Request(
        ENDPOINT,
        data=payload,
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
            "User-Agent": "omatrack-qt-docs-skill/1.0",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(http_request, timeout=30) as response:
            body = response.read().decode("utf-8")
            content_type = response.headers.get_content_type()
    except (urllib.error.URLError, TimeoutError) as error:
        raise RuntimeError(f"Qt documentation service request failed: {error}") from error

    if content_type == "text/event-stream":
        messages = [
            line.removeprefix("data: ")
            for line in body.splitlines()
            if line.startswith("data: ") and line != "data: "
        ]
        if not messages:
            raise RuntimeError("Qt documentation service returned no MCP message")
        result = json.loads(messages[-1])
    else:
        result = json.loads(body)

    if "error" in result:
        error = result["error"]
        raise RuntimeError(f"Qt documentation service error: {error}")
    return result["result"]


def add_common_search_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--query")
    parser.add_argument("--keywords", nargs="+")
    parser.add_argument("--version")
    parser.add_argument("--module")
    parser.add_argument("--product")
    parser.add_argument(
        "--filter", choices=("all", "class", "qml", "function", "guide")
    )
    parser.add_argument(
        "--intent",
        choices=("api", "tutorial", "guide", "concept", "example", "migration"),
    )
    parser.add_argument("--max-results", type=int, choices=range(1, 11), default=3)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Search and read official Qt documentation via Qt's MCP service."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    search = commands.add_parser("search", help="Search official Qt documentation")
    add_common_search_arguments(search)

    read = commands.add_parser("read", help="Read a documentation search result")
    read.add_argument("file", help="Filename returned by a search result")
    read.add_argument("--version")

    return parser.parse_args()


def nonempty(values: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in values.items() if value is not None}


def main() -> int:
    args = parse_arguments()
    if args.command == "search":
        if not args.query and not args.keywords:
            print("search requires --query or --keywords", file=sys.stderr)
            return 2
        arguments = nonempty(
            {
                "query": args.query,
                "keywords": args.keywords,
                "version": args.version,
                "module": args.module,
                "product": args.product,
                "filter": args.filter,
                "intent": args.intent,
                "max_results": args.max_results,
            }
        )
        tool_name = "qt_documentation_search"
    else:
        arguments = nonempty({"file": args.file, "version": args.version})
        tool_name = "qt_documentation_read"

    try:
        result = request("tools/call", {"name": tool_name, "arguments": arguments})
    except (RuntimeError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1

    for item in result.get("content", []):
        if item.get("type") == "text":
            print(item.get("text", ""))

    if result.get("isError"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
