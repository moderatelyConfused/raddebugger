// flows debuggee: C++ (virtual call, template, lambda). Exits with 50.

struct Shape
{
  virtual ~Shape() {}
  virtual int area() const = 0;
};

struct Rect : Shape
{
  int w, h;
  Rect(int w_, int h_) : w(w_), h(h_) {}
  int area() const override
  {
    int r = w * h;                    // @rect_area
    return r;
  }
};

template<typename T> __attribute__((noinline)) T twice(T v)
{
  T r = v + v;                        // @twice_body
  return r;
}

int main(int argc, char **argv)
{
  (void)argv;
  Rect rect(argc + 2, 4);             // 3 x 4 when run without arguments
  Shape *s = &rect;
  int a = s->area();                  // @cpp_virtual_call
  int t = twice(a);                   // @cpp_template_call
  auto lam = [&](int k) { return k + a; }; // @cpp_lambda_def
  int l = lam(2);                     // @cpp_lambda_call
  return a + t + l;                   // @cpp_exit  (12 + 24 + 14 = 50)
}
