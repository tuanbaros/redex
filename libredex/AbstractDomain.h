/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <type_traits>
#include <utility>

#include "Debug.h"

/*
 * This is an API for abstract domains, which are the fundamental structures in
 * Abstract Interpretation, as described in the seminal paper:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation: a unified lattice
 *  model for static analysis of programs by construction or approximation of
 *  fixpoints. In Conference Record of the Fourth Annual ACM SIGPLAN-SIGACT
 *  Symposium on Principles of Programming Languages, pages 238—252, 1977.
 *
 * Abstract domains were originally defined as lattices, but this is not a
 * hard requirement. As long as the join and meet operation are sound
 * approximations of the corresponding union and intersection operations on the
 * concrete domain, one can perform computations in a sound manner. Please see
 * the following paper for a description of more general Abstract Interpretation
 * frameworks:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation frameworks. Journal
 *  of Logic and Computation, 2(4):511—547, 1992.
 *
 * This API has been designed with performance in mind. This is why an element
 * of an abstract domain is mutable and all basic operations have side effects.
 * A functional interface is provided for convenience as a layer on top of these
 * operations. An abstract domain is thread safe, as all side-effecting
 * operations are guaranteed to be only invoked on thread-local objects. It is
 * the responsibility of the fixpoint operators to ensure that this assumption
 * is always verified.
 *
 * In order to avoid the use of type casts, the interface for abstract domains
 * is defined using the curiously recurring template pattern (CRTP). The final
 * class derived from an abstract domain is passed as a parameter to the base
 * domain. This guarantees that all operations carry the type of the derived
 * class. The standard usage is:
 *
 *   class MyDomain final : public AbstractDomain<MyDomain> { ... };
 *
 * All generic abstract domain combinators should follow the CRTP.
 *
 * Sample usage:
 *
 *  class MyDomain final : public AbstractDomain<MyDomain> {
 *   public:
 *    bool is_bottom() { ... }
 *    ...
 *  };
 *
 */
template <typename Derived>
class AbstractDomain {
 public:
  virtual ~AbstractDomain() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_base_of<AbstractDomain<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractDomain");
    static_assert(std::is_default_constructible<Derived>::value,
                  "Derived is not default constructible");
    static_assert(std::is_copy_constructible<Derived>::value,
                  "Derived is not copy constructible");
    static_assert(std::is_copy_assignable<Derived>::value,
                  "Derived is not copy assignable");
    // An abstract domain should implement the factory methods top() and
    // bottom() that respectively produce the top and bottom values.
    static_assert(std::is_same<decltype(Derived::bottom()), Derived>::value,
                  "Derived::bottom() does not exist");
    static_assert(std::is_same<decltype(Derived::top()), Derived>::value,
                  "Derived::top() does not exist");
  }

  virtual bool is_bottom() const = 0;

  virtual bool is_top() const = 0;

  /*
   * The partial order relation.
   */
  virtual bool leq(const Derived& other) const = 0;

  /*
   * a.equals(b) is semantically equivalent to a.leq(b) && b.leq(a).
   */
  virtual bool equals(const Derived& other) const = 0;

  /*
   * Elements of an abstract domain are mutable and the basic operations have
   * side effects.
   */

  virtual void set_to_bottom() = 0;

  virtual void set_to_top() = 0;

  /*
   * If the abstract domain is a lattice, this is the least upper bound
   * operation.
   */
  virtual void join_with(const Derived& other) = 0;

  /*
   * If the abstract domain has finite ascending chains, one doesn't need to
   * define a widening operator and can simply use the join instead.
   */
  virtual void widen_with(const Derived& other) = 0;

  /*
   * If the abstract domain is a lattice, this is the greatest lower bound
   * operation.
   */
  virtual void meet_with(const Derived& other) = 0;

  /*
   * If the abstract domain has finite descending chains, one doesn't need to
   * define a narrowing operator and can simply use the meet instead.
   */
  virtual void narrow_with(const Derived& other) = 0;

  /*
   * This is a functional interface on top of the side-effecting API provided
   * for convenience.
   */

  Derived join(const Derived& other) const {
    // Here and below: the static_cast is required in order to instruct
    // the compiler to use the copy constructor of the derived class.
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.join_with(other);
    return tmp;
  }

  Derived widening(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.widen_with(other);
    return tmp;
  }

  Derived meet(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.meet_with(other);
    return tmp;
  }

  Derived narrowing(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.narrow_with(other);
    return tmp;
  }
};

/*
 * When implementing an abstract domain, one often has to encode the Top and
 * Bottom values in a special way. This leads to a nontrivial case analysis when
 * writing the domain operations. The purpose of AbstractDomainScaffolding is to
 * implement this boilerplate logic provided a representation of regular
 * elements defined by the AbstractValue interface.
 */

/*
 * This interface represents the structure of the regular elements of an
 * abstract domain (like a constant, an interval, a points-to set, etc.).
 * Performing operations on those regular values may yield Top or Bottom, which
 * is why we define a Kind type to identify such situations.
 *
 * Sample usage:
 *
 *  class MyAbstractValue final : public AbstractValue<MyAbstractValue> {
 *   public:
 *    void clear() { m_table.clear(); }
 *    ...
 *   private:
 *    std::unordered_map<...> m_table;
 *  };
 *
 */
template <typename Derived>
class AbstractValue {
 public:
  enum class Kind { Bottom, Value, Top };

  virtual ~AbstractValue() {
    static_assert(std::is_base_of<AbstractValue<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractValue");
    static_assert(std::is_default_constructible<Derived>::value,
                  "Derived is not default constructible");
    static_assert(std::is_copy_constructible<Derived>::value,
                  "Derived is not copy constructible");
    static_assert(std::is_copy_assignable<Derived>::value,
                  "Derived is not copy assignable");
  }

  /*
   * When the result of an operation is Top or Bottom, we no longer need an
   * explicit representation for the abstract value. This method frees the
   * memory used to represent an abstract value (hash tables, vectors, etc.).
   */
  virtual void clear() = 0;

  /*
   * Even though we factor out the logic for Top and Bottom, these elements may
   * still be represented by regular abstract values (for example [0, -1] and
   * [-oo, +oo] in the domain of intervals), hence the need for such a method.
   */
  virtual Kind kind() const = 0;

  virtual bool leq(const Derived& other) const = 0;

  virtual bool equals(const Derived& other) const = 0;

  /*
   * These are the regular abstract domain operations that perform side effects.
   * They return a Kind value to identify situations where the result of the
   * operation is either Top or Bottom.
   */

  virtual Kind join_with(const Derived& other) = 0;

  virtual Kind widen_with(const Derived& other) = 0;

  virtual Kind meet_with(const Derived& other) = 0;

  virtual Kind narrow_with(const Derived& other) = 0;
};

/*
 * This abstract domain combinator takes an abstract value specification and
 * constructs a full-fledged abstract domain, handling all the logic for Top and
 * Bottom. It takes a poset and adds the two extremal elements Top and Bottom.
 * If the poset contains a Top and/or Bottom element, then those should be
 * coalesced with the extremal elements added by AbstractDomainScaffolding. This
 * is the purpose of the normalize() operation. It also explains why the lattice
 * operations return an AbstractValueKind: the scaffolding must be able to
 * identify when the result of one of these operations is an extremal element
 * that must be coalesced.
 *
 * Sample usage:
 *
 * class MyAbstractValue final : public AbstractValue<MyAbstractValue> {
 *  ...
 * };
 *
 * class MyAbstractDomain final
 *     : public AbstractDomainScaffolding<MyAbstractValue, MyAbstractDomain> {
 *  public:
 *   MyAbstractDomain(...) { ... }
 *
 *   // All basic operations defined in AbstractDomain are provided by
 *   // AbstractDomainScaffolding. We only need to define the operations
 *   // that are specific to MyAbstractDomain.
 *
 *   void my_operation(...) { ... }
 *
 *   void my_other_operation(...) { ... }
 * };
 *
 */
template <typename Value, typename Derived>
class AbstractDomainScaffolding : public AbstractDomain<Derived> {
 public:
  using AbstractValueKind = typename AbstractValue<Value>::Kind;

  virtual ~AbstractDomainScaffolding() {
    static_assert(std::is_base_of<AbstractValue<Value>, Value>::value,
                  "Value doesn't inherit from AbstractValue");
    static_assert(std::is_base_of<AbstractDomainScaffolding<Value, Derived>,
                                  Derived>::value,
                  "Derived doesn't inherit from AbstractDomainScaffolding");
  }

  /*
   * The choice of lattice element returned by the default constructor is
   * completely arbitrary. In pratice though, the abstract value used to
   * initiate a fixpoint iteration is most often constructed in this way.
   */
  AbstractDomainScaffolding() { m_kind = m_value.kind(); }

  /*
   * A convenience constructor for creating Bottom and Top.
   */
  explicit AbstractDomainScaffolding(AbstractValueKind kind) : m_kind(kind) {
    always_assert(kind != AbstractValueKind::Value);
  }

  AbstractValueKind kind() const { return m_kind; }

  bool is_bottom() const override {
    return m_kind == AbstractValueKind::Bottom;
  }

  bool is_top() const override { return m_kind == AbstractValueKind::Top; }

  bool is_value() const { return m_kind == AbstractValueKind::Value; }

  void set_to_bottom() override {
    m_kind = AbstractValueKind::Bottom;
    m_value.clear();
  }

  void set_to_top() override {
    m_kind = AbstractValueKind::Top;
    m_value.clear();
  }

  bool leq(const Derived& other) const override {
    if (is_bottom()) {
      return true;
    }
    if (other.is_bottom()) {
      return false;
    }
    if (other.is_top()) {
      return true;
    }
    if (is_top()) {
      return false;
    }
    assert(m_kind == AbstractValueKind::Value &&
           other.m_kind == AbstractValueKind::Value);
    return m_value.leq(other.m_value);
  }

  bool equals(const Derived& other) const override {
    return m_kind == other.m_kind && m_value.equals(other.m_value);
  }

  void join_with(const Derived& other) override {
    auto value_join = [this, &other]() {
      m_kind = m_value.join_with(other.m_value);
    };
    join_like_operation_with(other, value_join);
  }

  void widen_with(const Derived& other) override {
    auto value_widening = [this, &other]() {
      m_kind = m_value.widen_with(other.m_value);
    };
    join_like_operation_with(other, value_widening);
  }

  void meet_with(const Derived& other) override {
    auto value_meet = [this, &other]() {
      m_kind = m_value.meet_with(other.m_value);
    };
    meet_like_operation_with(other, value_meet);
  }

  void narrow_with(const Derived& other) override {
    auto value_narrowing = [this, &other]() {
      m_kind = m_value.narrow_with(other.m_value);
    };
    meet_like_operation_with(other, value_narrowing);
  }

 protected:
  // These methods allow the derived class to manipulate the abstract value.
  Value* get_value() { return &m_value; }

  const Value* get_value() const { return &m_value; }

  void set_to_value(const Value& value) {
    m_kind = value.kind();
    m_value = value;
  }

  // This method is used to normalize the representation when the abstract value
  // can denote Top and/or Bottom.
  void normalize() {
    m_kind = m_value.kind();
    if (m_kind == AbstractValueKind::Bottom ||
        m_kind == AbstractValueKind::Top) {
      m_value.clear();
    }
  }

 private:
  void join_like_operation_with(const Derived& other,
                                std::function<void()> operation) {
    if (is_top() || other.is_bottom()) {
      return;
    }
    if (other.is_top()) {
      set_to_top();
      return;
    }
    if (is_bottom()) {
      m_kind = other.m_kind;
      m_value = other.m_value;
      return;
    }
    operation();
  }

  void meet_like_operation_with(const Derived& other,
                                std::function<void()> operation) {
    if (is_bottom() || other.is_top()) {
      return;
    }
    if (other.is_bottom()) {
      set_to_bottom();
      return;
    }
    if (is_top()) {
      m_kind = other.m_kind;
      m_value = other.m_value;
      return;
    }
    operation();
  }

  AbstractValueKind m_kind;
  Value m_value;
};
