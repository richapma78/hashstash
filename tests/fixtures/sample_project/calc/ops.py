"""Arithmetic helpers."""


def add(a, b):
    return a + b


def mul(a, b):
    total = 0
    for _ in range(b):
        total = add(total, a)
    return total


class Calculator:
    def __init__(self):
        self.memory = 0

    def apply(self, op, x):
        self.memory = op(self.memory, x)
        return self.memory

    def clear(self):
        self.memory = 0
