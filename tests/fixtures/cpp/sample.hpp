#ifndef SAMPLE_HPP
#define SAMPLE_HPP

#include <string>
#include "util.h"

namespace demo {

// A point in 2D.
struct Point {
  int x = 0, y = 0;
  int sum() const { return x + y; }
};

class Shape {
 public:
  virtual ~Shape() = default;
  virtual double area() const = 0;

  /// Scales the shape.
  void scale(double f) {
    factor_ *= f;
    normalize();
  }

 private:
  void normalize();
  double factor_ = 1;
};

enum class Color { Red, Green };

template <typename T>
T twice(T v) {
  return v + v;
}

int add(int a, int b);
int add(int a, int b) { return a + b; }
double add(double a, double b) { return a + b; }

void Shape::normalize() {
  if (factor_ < 0) factor_ = -factor_;
  helper::clamp(factor_);
}

}  // namespace demo

typedef struct {
  int a;
} Anonymous;

typedef struct Named {
  int b;
} Named;

#endif
