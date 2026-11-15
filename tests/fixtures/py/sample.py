#!/usr/bin/env python3
"""Sample module for hashstash round-trip tests."""

import os
import sys as system
from collections import OrderedDict
from .helpers import tidy, shout as yell

CONSTANT = 42  # trailing comment


# A comment attached to greet.
# Second line of the comment.
def greet(name: str) -> str:
    """Return a greeting."""
    return f"hello {name}"


@decorator
@another(arg=1)
def decorated(x):
    return tidy(x) + yell(x)


class Server(Base):
    """A server."""

    port = 8080

    def __init__(self, port):
        self.port = port

    # Comment above start.
    @property
    def address(self):
        return os.path.join("host", str(self.port))

    def start(self):
        self.prepare()
        return greet("server")

    def prepare(self):
        pass

    class Config:
        debug = False

        def toggle(self):
            self.debug = not self.debug


async def fetch(url):
    data = await get(url)
    return data


def one_liner(): return 1


if __name__ == "__main__":
    server = Server(CONSTANT)
    server.start()
    system.exit(0)
