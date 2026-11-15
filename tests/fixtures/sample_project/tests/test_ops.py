from calc.ops import add, mul, Calculator


def test_add():
    assert add(2, 3) == 5


def test_add_negative():
    assert add(-2, 1) == -1


def test_mul():
    assert mul(4, 3) == 12


class TestCalculator:
    def test_apply(self):
        c = Calculator()
        assert c.apply(add, 5) == 5
