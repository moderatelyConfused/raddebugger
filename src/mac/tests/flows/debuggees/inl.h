static inline int inl_add(volatile int *p, int v)
{
  *p += v;
  *p += 1;
  return *p;
}
