// Swift corpus program: structs, classes, protocols, generics, closures,
// optionals, enums with payloads, error handling.
import Foundation

protocol Shape {
  func area() -> Int64
  var name: String { get }
}

struct Rect: Shape {
  var w: Int32
  var h: Int32
  func area() -> Int64 { return Int64(w) * Int64(h) }
  var name: String { return "rect" }
}

final class Circle: Shape {
  let r: Int32
  init(r: Int32) { self.r = r }
  func area() -> Int64 { return 3 * Int64(r) * Int64(r) }
  var name: String { return "circle" }
}

enum ParseResult {
  case value(Int32)
  case failure(String)
}

enum DivideError: Error {
  case byZero
}

func checkedDivide(_ a: Int32, _ b: Int32) throws -> Int32 {
  if b == 0 { throw DivideError.byZero }
  return a / b
}

func sumSquares<T: BinaryInteger>(_ items: [T]) -> Int64 {
  var sum: Int64 = 0
  for v in items {
    let x = Int64(v)
    sum += x * x
  }
  return sum
}

@inline(never)
func totalArea(_ shapes: [Shape]) -> Int64 {
  var total: Int64 = 0
  for s in shapes {
    total += s.area()
    total += Int64(s.name.utf8.count)
  }
  return total
}

func parse(_ s: String) -> ParseResult {
  if let v = Int32(s) { return .value(v) }
  return .failure("not a number: \(s)")
}

var gCounter: Int32 = 42

let seed = Int32(CommandLine.arguments.count) + gCounter
let ints: [Int32] = (0..<seed).map { $0 - 3 }
let bytes: [UInt8] = (0..<UInt8(seed & 0x7f)).map { $0 }
let s1 = sumSquares(ints)
let s2 = sumSquares(bytes)

let shapes: [Shape] = [Rect(w: seed, h: seed + 1), Circle(r: seed)]
let areas = totalArea(shapes)

let captured = seed * 3
let scale: (Int32) -> Int32 = { v in v * captured }
var acc: Int32 = 0
let bump: (Int32) -> Int32 = { v in acc += v; return acc }
let l0 = scale(seed)
let l1 = bump(seed) + bump(1)

var divTotal: Int32 = 0
for d in -2..<3 {
  do {
    divTotal += try checkedDivide(seed * 7, Int32(d))
  } catch {
    divTotal += 11
  }
}

var parsed: Int32 = 0
for s in ["12", "x", "\(seed)"] {
  switch parse(s) {
  case .value(let v): parsed += v
  case .failure(let msg): parsed += Int32(msg.utf8.count)
  }
}

gCounter += 1
print("swiftprog: s1=\(s1) s2=\(s2) areas=\(areas) l=\(l0),\(l1) div=\(divTotal) parsed=\(parsed) g=\(gCounter)")
exit((Int32(truncatingIfNeeded: s1) + l0 + l1 + divTotal) & 0x7f)
