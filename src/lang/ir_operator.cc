/*!
 *  Copyright (c) 2017 by Contributors
 * \file ir_operator.cc
 */
#include <tvm/base.h>
#include <tvm/ir.h>
#include <tvm/ir_operator.h>
#include <cmath>

namespace tvm {

/*!
 * \brief Check whether type is used to represent index.
 *
 * Index types are frequently used in shape computation
 * and need to be aggressively constant-folded.
 *
 * \param type The type to represent index.
 * \return the checked result.
 */
inline bool IsIndexType(const Type& type) {
  return type.is_int() && type.lanes() == 1 &&
      (type.bits() == 32 || type.bits() == 64);
}

// simple cast that only checks if type matches and cast
inline Expr SimpleCast(const Type& t, Expr value) {
  if (value.type() == t) return value;
  return ir::Cast::make(t, value);
}

// The public function with a quick checking path.
void BinaryOpMatchTypes(Expr& lhs, Expr& rhs) {  // NOLINT(*)
  if (lhs.type() == rhs.type()) return;
  Type ltype = lhs.type();
  Type rtype = rhs.type();
  if (ltype.lanes() == 1 && rtype.lanes() != 1) {
    lhs = ir::Broadcast::make(lhs, rtype.lanes());
  } else if (rtype.lanes() == 1 && ltype.lanes() != 1) {
    rhs = ir::Broadcast::make(rhs, ltype.lanes());
  } else {
    CHECK(ltype.lanes() == rtype.lanes())
        << "Cannot match type " << ltype << " vs " << rtype;
  }
  if (lhs.type() == rhs.type()) return;
  // Only do very simple type coversion
  // int->float, int(32)->int(64)
  // require the types to be relatively consistent
  // This will the reduce amount code generated by operators
  // and also help user to find potential type conversion problems.
  if (!lhs.type().is_float() && rhs.type().is_float()) {
    // int->float
    lhs = cast(rhs.type(), lhs);
  } else if (lhs.type().is_float() && !rhs.type().is_float()) {
    // int->float
    rhs = cast(lhs.type(), rhs);
  } else if ((lhs.type().is_int() && rhs.type().is_int()) ||
             (lhs.type().is_uint() && rhs.type().is_uint())) {
    // promote int to higher bits
    if (lhs.type().bits() < rhs.type().bits()) {
      lhs = cast(rhs.type(), lhs);
    } else {
      rhs = cast(lhs.type(), rhs);
    }
  } else if ((lhs.type().is_int() && rhs.type().is_uint()) ||
             (lhs.type().is_uint() && rhs.type().is_int())) {
    int bits = std::max(lhs.type().bits(), rhs.type().bits());
    lhs = SimpleCast(Int(bits, lhs.type().lanes()), lhs);
    rhs = SimpleCast(Int(bits, rhs.type().lanes()), rhs);
  } else {
    LOG(FATAL) << "Cannot match type " << ltype << " vs " << rtype;
  }
}


template<typename ValueType>
inline bool ConstPowerHelper(ValueType val, int *shift) {
  if (val <= 0) return false;
  shift[0] = 0;
  while (val != 0) {
    if (val & 1) {
      return (val == 1);
    }
    ++shift[0];
    val = val >> 1;
  }
  return true;
}

bool is_const_power_of_two_integer(const Expr& x, int* shift) {
  if (const auto* op = x.as<ir::IntImm>()) {
    return ConstPowerHelper(op->value, shift);
  } else if (const auto* op = x.as<ir::UIntImm>()) {
    return ConstPowerHelper(op->value, shift);
  } else {
    return false;
  }
}

Expr cast(const Type& t, Expr value) {
  using ir::IntImm;
  using ir::FloatImm;
  if (value.type() == t) return value;
  // const fold IntImm as they are used in index computations
  if (t.lanes() == 1) {
    if (const IntImm* op = value.as<IntImm>()) {
      return make_const(t, op->value);
    } else if (const FloatImm* op = value.as<FloatImm>()) {
      return make_const(t, op->value);
    }
    return ir::Cast::make(t, value);
  } else {
    if (value.type().lanes() == 1) {
      // manually unroll cast
      Type vtype = t.element_of();
      if (value.type() != vtype) {
        if (const IntImm* op = value.as<IntImm>()) {
          value = make_const(vtype, op->value);
        } else if (const FloatImm* op = value.as<FloatImm>()) {
          value = make_const(vtype, op->value);
        } else {
          value = ir::Cast::make(vtype, value);
        }
      }
      return ir::Broadcast::make(value, t.lanes());
    } else {
      CHECK(value.type().lanes() == t.lanes());
      return ir::Cast::make(t, value);
    }
  }
}

Expr reinterpret(const Type& t, Expr value) {
  if (value.type() == t) return value;
  return ir::Call::make(t, ir::Call::reinterpret, { value }, ir::Call::PureIntrinsic);
}

#define TVM_INDEX_CONST_PROPAGATION(BODY)                               \
  using ir::IntImm;                                                     \
  using ir::UIntImm;                                                    \
  const IntImm* pa = a.as<IntImm>();                                    \
  const IntImm* pb = b.as<IntImm>();                                    \
  const Type& ta = a.type();                                            \
  const Type& tb = b.type();                                            \
  if (IsIndexType(ta) && IsIndexType(tb)) {                             \
    BODY;                                                               \
  }                                                                     \
  BinaryOpMatchTypes(a, b);

#define TVM_ARITH_CONST_PROPAGATION(BODY)                               \
  using ir::IntImm;                                                     \
  using ir::UIntImm;                                                    \
  using ir::FloatImm;                                                   \
  BinaryOpMatchTypes(a, b);                                             \
  const IntImm* pa = a.as<IntImm>();                                    \
  const IntImm* pb = b.as<IntImm>();                                    \
  const FloatImm* fa = a.as<FloatImm>();                                \
  const FloatImm* fb = b.as<FloatImm>();                                \
  BODY;


Expr operator+(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, pa->value + pb->value);
      if (pa && pa->value == 0) return SimpleCast(rtype, b);
      if (pb && pb->value == 0) return SimpleCast(rtype, a);
      if (fa && fb) return FloatImm::make(rtype, fa->value + fb->value);
      if (fa && fa->value == 0) return SimpleCast(rtype, b);
      if (fb && fb->value == 0) return SimpleCast(rtype, a);
    });
  return ir::Add::make(a, b);
}

Expr operator-(Expr a) {
  using ir::IntImm;
  using ir::FloatImm;
  const IntImm* pa = a.as<IntImm>();
  const FloatImm* fa = a.as<FloatImm>();
  if (pa) return ir::IntImm::make(a.type(), -pa->value);
  if (fa) return ir::FloatImm::make(a.type(), -fa->value);
  return make_zero(a.type()) - a;
}

Expr operator-(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, pa->value - pb->value);
      if (pb && pb->value == 0) return SimpleCast(rtype, a);
      if (fa && fb) return FloatImm::make(rtype, fa->value - fb->value);
      if (fb && fb->value == 0) return SimpleCast(rtype, a);
    });
  return ir::Sub::make(a, b);
}

Expr operator*(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, pa->value * pb->value);
      if (pa) {
        if (pa->value == 1) return SimpleCast(rtype, b);
        if (pa->value == 0) return SimpleCast(rtype, a);
      }
      if (pb) {
        if (pb->value == 1) return SimpleCast(rtype, a);
        if (pb->value == 0) return SimpleCast(rtype, b);
      }
      if (fa && fb) return FloatImm::make(rtype, fa->value * fb->value);
      if (fa) {
        if (fa->value == 1) return SimpleCast(rtype, b);
        if (fa->value == 0) return SimpleCast(rtype, a);
      }
      if (fb) {
        if (fb->value == 1) return SimpleCast(rtype, a);
        if (fb->value == 0) return SimpleCast(rtype, b);
      }
    });
  return ir::Mul::make(a, b);
}

Expr operator/(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      // due to division and mod can have different modes
      // only constant fold positive number where rule is fixed.
      if (pa && pb && pa->value >= 0 && pb->value > 0) {
        return IntImm::make(rtype, pa->value / pb->value);
      }
      if (pa) {
        if (pa->value == 0) return SimpleCast(rtype, a);
      }
      if (pb) {
        if (pb->value == 1) return SimpleCast(rtype, a);
        CHECK_NE(pb->value, 0) << "Divide by zero";
      }
      if (fa && fb && fb->value != 0) {
        return FloatImm::make(rtype, fa->value / fb->value);
      }
      if (fa && fa->value == 0) {
        return SimpleCast(rtype, a);
      }
      if (fb) {
        if (fb->value == 1) return SimpleCast(rtype, a);
        CHECK_NE(fb->value, 0) << "Divide by zero";
      }
    });
  return ir::Div::make(a, b);
}

Expr operator%(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      // due to division and mod can have different modes
      // only constant fold positive number where rule is fixed.
      if (pa && pb && pa->value >= 0 && pb->value > 0) {
        return IntImm::make(rtype, pa->value % pb->value);
      }
      if (pa) {
        if (pa->value == 0) return SimpleCast(rtype, a);
      }
      if (pb) {
        if (pb->value == 1) return make_zero(rtype);
        CHECK_NE(pb->value, 0) << "Divide by zero";
      }
    });
  return ir::Mod::make(a, b);
}

Expr min(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, std::min(pa->value, pb->value));
      if (fa && fb) return FloatImm::make(rtype, std::min(fa->value, fb->value));
    });
  return ir::Min::make(a, b);
}

Expr max(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      const Type& ta = a.type();
      const Type& tb = b.type();
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, std::max(pa->value, pb->value));
      if (fa && fb) return FloatImm::make(rtype, std::max(fa->value, fb->value));
    });
  return ir::Max::make(a, b);
}

Expr if_then_else(Expr cond, Expr true_value, Expr false_value) {
  using ir::IntImm;
  using ir::UIntImm;
  CHECK(cond.type() == Bool(1))
      << "if_then_else only accept a single condition";
  BinaryOpMatchTypes(true_value, false_value);
  if (const UIntImm* op = cond.as<UIntImm>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  } else if (const IntImm* op = cond.as<IntImm>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  }
  return ir::Call::make(
      true_value.type(),
      ir::intrinsic::tvm_if_then_else,
      {cond, true_value, false_value},
      ir::Call::PureIntrinsic);
}

Expr likely(Expr cond) {
  if (is_const(cond)) return cond;
  return ir::Call::make(cond.type(), ir::Call::likely, { cond }, ir::Call::PureIntrinsic);
}

Expr operator>(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value > pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value > fb->value);
    });
  return ir::GT::make(a, b);
}

Expr operator>=(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value >= pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value >= fb->value);
    });
  return ir::GE::make(a, b);
}

Expr operator<(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value < pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value < fb->value);
    });
  return ir::LT::make(a, b);
}

Expr operator<=(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value <= pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value <= fb->value);
    });
  return ir::LE::make(a, b);
}

Expr operator==(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value == pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value == fb->value);
    });
  return ir::EQ::make(a, b);
}

Expr operator!=(Expr a, Expr b) {
  TVM_ARITH_CONST_PROPAGATION({
      if (pa && pb) return UIntImm::make(UInt(1), pa->value != pb->value);
      if (fa && fb) return UIntImm::make(UInt(1), fa->value != fb->value);
    });
  return ir::NE::make(a, b);
}

Expr operator&&(Expr a, Expr b) {
  using ir::UIntImm;
  if (a.type().is_bool() && b.type().is_bool()) {
    const UIntImm* pa = a.as<UIntImm>();
    const UIntImm* pb = b.as<UIntImm>();
    if (pa && pa->value) return b;
    if (pa && !pa->value) return a;
    if (pb && pb->value) return a;
    if (pb && !pb->value) return b;
  }
  return ir::And::make(a, b);
}

Expr operator||(Expr a, Expr b) {
  using ir::UIntImm;
  if (a.type().is_bool() && b.type().is_bool()) {
    const UIntImm* pa = a.as<UIntImm>();
    const UIntImm* pb = b.as<UIntImm>();
    if (pa && pa->value) return a;
    if (pa && !pa->value) return b;
    if (pb && pb->value) return b;
    if (pb && !pb->value) return a;
  }
  return ir::Or::make(a, b);
}

Expr operator!(Expr a) {
  using ir::UIntImm;
  const UIntImm* pa = a.as<UIntImm>();
  if (pa) {
    return UIntImm::make(UInt(1), !(pa->value));
  }
  return ir::Not::make(a);
}

Expr operator>>(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, (pa->value >> pb->value));
      if (pb) {
        if (pb->value == 0) return SimpleCast(rtype, a);
      }
    });
  return ir::Call::make(a.type(), ir::Call::shift_right, { a, b }, ir::Call::PureIntrinsic);
}

Expr operator<<(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, (pa->value << pb->value));
      if (pb) {
        if (pb->value == 0) return SimpleCast(rtype, a);
      }
    });
  return ir::Call::make(a.type(), ir::Call::shift_left, { a, b }, ir::Call::PureIntrinsic);
}

Expr operator&(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, (pa->value & pb->value));
    });
  return ir::Call::make(a.type(), ir::Call::bitwise_and, { a, b }, ir::Call::PureIntrinsic);
}

Expr operator|(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, (pa->value | pb->value));
    });
  return ir::Call::make(a.type(), ir::Call::bitwise_or, { a, b }, ir::Call::PureIntrinsic);
}

Expr operator^(Expr a, Expr b) {
  TVM_INDEX_CONST_PROPAGATION({
      Type rtype = ta.bits() >= tb.bits() ? ta : tb;
      if (pa && pb) return IntImm::make(rtype, (pa->value ^ pb->value));
    });
  return ir::Call::make(a.type(), ir::Call::bitwise_xor, { a, b }, ir::Call::PureIntrinsic);
}

Expr operator~(Expr a) {
  CHECK(a.type().is_int() || a.type().is_uint());
  return ir::Call::make(a.type(), ir::Call::bitwise_not, { a }, ir::Call::PureIntrinsic);
}

Expr pow(Expr x, Expr y) {
  BinaryOpMatchTypes(x, y);
  CHECK(x.type().is_float()) << "power only applies to float";
  return ir::Call::make(x.type(), "pow", { x, y }, ir::Call::PureIntrinsic);
}

Expr abs(Expr x) {
  if (x.type().is_int()) {
    using ir::IntImm;
    const IntImm* px = x.as<IntImm>();
    if (px) {
      return ir::IntImm::make(x.type(), std::abs(px->value));
    }
    return ir::Select::make(x >= make_zero(x.type()), x, -x);
  } else if (x.type().is_float()) {
    using ir::FloatImm;
    const FloatImm* fx = x.as<FloatImm>();
    if (fx) {
      return ir::FloatImm::make(x.type(), std::fabs(fx->value));
    }
    return ir::Call::make(x.type(), "fabs", {x}, ir::Call::PureIntrinsic);
  } else if (x.type().is_uint()) {
    return x;
  } else {
    LOG(FATAL) << "Data type " << x.type()
               <<" not supported for absolute op. Skipping absolute op...";
    return x;
  }
}

Expr sum(Expr source, Array<IterVar> rdom) {
  Var x("x", source.type()), y("y", source.type());
  Expr result = ir::Add::make(x, y);
  Expr identity_element = make_zero(source.type());
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::Reduce::make(combiner, {source}, rdom, make_const(Bool(1), true), 0);
}

Expr max(Expr source, Array<IterVar> rdom) {
  Var x("x", source.type()), y("y", source.type());
  Expr result = ir::Max::make(x, y);
  Expr identity_element = source.type().min();
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::Reduce::make(combiner, {source}, rdom, make_const(Bool(1), true), 0);
}

Expr min(Expr source, Array<IterVar> rdom) {
  Var x("x", source.type()), y("y", source.type());
  Expr result = ir::Min::make(x, y);
  Expr identity_element = source.type().max();
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::Reduce::make(combiner, {source}, rdom, make_const(Bool(1), true), 0);
}

Expr prod(Expr source, Array<IterVar> rdom) {
  Var x("x", source.type()), y("y", source.type());
  Expr result = ir::Mul::make(x, y);
  Expr identity_element = make_const(source.type(), 1);
  ir::CommReducer combiner =
    ir::CommReducerNode::make({x}, {y}, {result}, {identity_element});
  return ir::Reduce::make(combiner, {source}, rdom, make_const(Bool(1), true), 0);
}

Expr fmod(Expr x, Expr y) {
  BinaryOpMatchTypes(x, y);
  CHECK(x.type().is_float()) << "fmod only applies to float";
  return ir::Call::make(x.type(), "fmod", { x, y }, ir::Call::PureIntrinsic);
}

Expr floor(Expr x) {
  using ir::FloatImm;
  const FloatImm* fx = x.as<FloatImm>();
  if (fx) return FloatImm::make(x.type(), std::floor(fx->value));
  return ir::Call::make(x.type(), "floor", {x}, ir::Call::PureIntrinsic);
}

Expr ceil(Expr x) {
  using ir::FloatImm;
  const FloatImm* fx = x.as<FloatImm>();
  if (fx) return FloatImm::make(x.type(), std::ceil(fx->value));
  return ir::Call::make(x.type(), "ceil", {x}, ir::Call::PureIntrinsic);
}

Expr round(Expr x) {
  using ir::FloatImm;
  const FloatImm* fx = x.as<FloatImm>();
  if (fx) return FloatImm::make(x.type(), std::nearbyint(fx->value));
  return ir::Call::make(x.type(), "round", {x}, ir::Call::PureIntrinsic);
}

Expr trunc(Expr x) {
  using ir::FloatImm;
  const FloatImm* fx = x.as<FloatImm>();
  if (fx) {
    return FloatImm::make(x.type(), (fx->value < 0 ? std::ceil(fx->value) :
                                     std::floor(fx->value)));
  }
  return ir::Call::make(x.type(), "trunc", {x}, ir::Call::PureIntrinsic);
}

}  // namespace tvm