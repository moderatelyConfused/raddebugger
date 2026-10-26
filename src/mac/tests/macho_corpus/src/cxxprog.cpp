// C++ corpus program: templates, inline member functions, virtual calls,
// exceptions, lambdas, namespaces, a static member, operator overloading.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <functional>
#include <memory>

namespace geom
{
  struct Point
  {
    int32_t x;
    int32_t y;
    Point operator+(const Point &o) const { return Point{x + o.x, y + o.y}; }
    int32_t manhattan() const { return (x < 0 ? -x : x) + (y < 0 ? -y : y); }
  };

  class Shape
  {
  public:
    virtual ~Shape() {}
    virtual int64_t area() const = 0;
    virtual const char *name() const { return "shape"; }
    static int32_t s_live_count;
  };
  int32_t Shape::s_live_count = 0;

  class Rect : public Shape
  {
  public:
    Rect(int32_t w, int32_t h) : w_(w), h_(h) { s_live_count += 1; }
    ~Rect() override { s_live_count -= 1; }
    int64_t area() const override { return (int64_t)w_ * h_; }
    const char *name() const override { return "rect"; }
  private:
    int32_t w_, h_;
  };

  class Circle : public Shape
  {
  public:
    explicit Circle(int32_t r) : r_(r) { s_live_count += 1; }
    ~Circle() override { s_live_count -= 1; }
    int64_t area() const override { return (int64_t)3 * r_ * r_; }
  private:
    int32_t r_;
  };
}

template<typename T>
T accumulate_squares(const std::vector<T> &v)
{
  T sum = T();
  for(const T &e : v)
  {
    sum += e * e;
  }
  return sum;
}

template<typename T, int N>
struct Ring
{
  T data[N];
  int head = 0;
  void push(T v) { data[head] = v; head = (head + 1) % N; }
  T at(int i) const { return data[(head + i) % N]; }
};

static int32_t
checked_divide(int32_t a, int32_t b)
{
  if(b == 0)
  {
    throw std::invalid_argument("divide by zero");
  }
  return a / b;
}

__attribute__((noinline)) static int32_t
try_divides(int32_t seed)
{
  int32_t total = 0;
  for(int32_t i = -2; i < 3; i += 1)
  {
    try
    {
      total += checked_divide(seed * 7, i);
    }
    catch(const std::invalid_argument &e)
    {
      total += (int32_t)std::strlen(e.what());
    }
  }
  return total;
}

__attribute__((noinline)) static int64_t
sum_areas(const std::vector<std::unique_ptr<geom::Shape>> &shapes)
{
  int64_t total = 0;
  for(const auto &s : shapes)
  {
    total += s->area();           // virtual call
    total += (int64_t)s->name()[0];
  }
  return total;
}

thread_local int32_t t_cxx_counter = 1;
int32_t g_cxx_global = 42;

int
main(int argc, char **argv)
{
  int32_t seed = argc + g_cxx_global;
  std::vector<int32_t> ints;
  std::vector<double> dbls;
  for(int32_t i = 0; i < seed; i += 1)
  {
    ints.push_back(i - 3);
    dbls.push_back(i * 0.5);
  }
  int32_t isum = accumulate_squares(ints);
  double dsum = accumulate_squares(dbls);

  Ring<int32_t, 5> ring;
  for(int32_t i = 0; i < seed * 2; i += 1) { ring.push(i * seed); }

  std::vector<std::unique_ptr<geom::Shape>> shapes;
  shapes.push_back(std::make_unique<geom::Rect>(seed, seed + 1));
  shapes.push_back(std::make_unique<geom::Circle>(seed));
  int64_t areas = sum_areas(shapes);

  int32_t captured = seed * 3;
  auto scale = [captured](int32_t v) { return v * captured; };          // lambda, by copy
  auto bump = [&captured](int32_t v) { captured += v; return captured; }; // lambda, by ref
  std::function<int32_t(int32_t)> fn = scale;
  int32_t l0 = fn(ring.at(1));
  int32_t l1 = bump(ring.at(2));

  geom::Point p{seed, -seed};
  geom::Point q = p + geom::Point{2, 3};
  int32_t div_total = try_divides(seed);
  t_cxx_counter += argc;

  std::printf("%s: isum=%d dsum=%g areas=%lld l=%d,%d q=%d live=%d div=%d tl=%d\n",
              argv[0], isum, dsum, (long long)areas, l0, l1, q.manhattan(),
              geom::Shape::s_live_count, div_total, t_cxx_counter);
  return (isum + l0 + l1 + div_total) & 0x7f;
}
