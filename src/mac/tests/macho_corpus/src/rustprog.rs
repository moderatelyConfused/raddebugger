// Rust corpus program: generics, trait objects, closures, iterators, a thread
// and a thread_local!, Option/Result, catch_unwind.
use std::cell::Cell;
use std::panic;
use std::thread;

thread_local! {
    static T_COUNTER: Cell<i32> = Cell::new(5);
}

static mut G_CALLS: i32 = 0;
static G_NAME: &str = "rustprog";

trait Shape {
    fn area(&self) -> i64;
    fn name(&self) -> &'static str {
        "shape"
    }
}

struct Rect {
    w: i32,
    h: i32,
}

struct Circle {
    r: i32,
}

impl Shape for Rect {
    fn area(&self) -> i64 {
        (self.w as i64) * (self.h as i64)
    }
    fn name(&self) -> &'static str {
        "rect"
    }
}

impl Shape for Circle {
    fn area(&self) -> i64 {
        3 * (self.r as i64) * (self.r as i64)
    }
}

fn sum_squares<T>(items: &[T]) -> i64
where
    T: Copy + Into<i64>,
{
    items.iter().map(|&v| v.into()).map(|v: i64| v * v).sum()
}

#[inline(never)]
fn checked_div(a: i32, b: i32) -> Result<i32, String> {
    if b == 0 {
        Err(String::from("divide by zero"))
    } else {
        Ok(a / b)
    }
}

#[inline(never)]
fn total_area(shapes: &[Box<dyn Shape>]) -> i64 {
    let mut total = 0;
    for s in shapes {
        total += s.area();
        total += s.name().len() as i64;
        unsafe {
            G_CALLS += 1;
        }
    }
    total
}

fn bump_thread_local(by: i32) -> i32 {
    T_COUNTER.with(|c| {
        c.set(c.get() + by);
        c.get()
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let seed = args.len() as i32 + 2;

    let ints: Vec<i32> = (0..seed).map(|i| i - 1).collect();
    let bytes: Vec<u8> = (0..seed as u8).collect();
    let s1 = sum_squares(&ints);
    let s2 = sum_squares(&bytes);

    let shapes: Vec<Box<dyn Shape>> = vec![
        Box::new(Rect { w: seed, h: seed + 1 }),
        Box::new(Circle { r: seed }),
    ];
    let areas = total_area(&shapes);

    let captured = seed * 3;
    let scale = move |v: i32| v * captured;
    let mut acc = 0;
    let mut bump = |v: i32| {
        acc += v;
        acc
    };
    let l0 = scale(seed);
    let l1 = bump(seed) + bump(1);

    let divs: Vec<Result<i32, String>> = (-2..3).map(|d| checked_div(seed * 7, d)).collect();
    let div_total: i32 = divs
        .iter()
        .map(|r| match r {
            Ok(v) => *v,
            Err(e) => e.len() as i32,
        })
        .sum();

    let tl_main = bump_thread_local(seed);
    let handle = thread::spawn(move || bump_thread_local(seed * 2));
    let tl_thread = handle.join().unwrap_or(-1);

    let caught = panic::catch_unwind(|| {
        if seed > 1 {
            panic!("expected panic");
        }
        0
    });
    let panicked = caught.is_err() as i32;

    let calls = unsafe { G_CALLS };
    println!(
        "{}: s1={} s2={} areas={} l={},{} div={} tl={},{} panicked={} calls={}",
        G_NAME, s1, s2, areas, l0, l1, div_total, tl_main, tl_thread, panicked, calls
    );
    std::process::exit(((s1 + l0 as i64 + l1 as i64 + div_total as i64) & 0x7f) as i32);
}
