#ifndef TTG_H_INCLUDED
#define TTG_H_INCLUDED

#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <boost/callable_traits.hpp>  // needed for wrap.h

#include "util/demangle.h"
#include "util/meta.h"

namespace ttg {

  namespace overload {
    /// Computes unique hash values for objects of type T.
    /// Overload for your type.
    template <typename Output, typename Input>
    Output unique_hash(const Input &t);
  }  // namespace overload
  using namespace ::ttg::overload;

  namespace detail {

    /// the default keymap implementation requires std::hash{}(key) ... use SFINAE
    /// TODO improve error messaging via more elaborate techniques e.g.
    /// https://gracicot.github.io/tricks/2017/07/01/deleted-function-diagnostic.html
    template <typename keyT, typename Enabler = void>
    struct default_keymap_impl;
    template <typename keyT>
    struct default_keymap_impl<keyT,
                               ::ttg::meta::void_t<decltype(std::declval<std::hash<keyT>>()(std::declval<const keyT&>()))>> {
      default_keymap_impl() = default;
      default_keymap_impl(int world_size) : world_size(world_size) {}
      // clang-format on
      int operator()(const keyT &key) const { return std::hash<keyT>{}(key) % world_size; }

     private:
      int world_size;
    };

  }  // namespace detail

  namespace detail {
    bool &trace_accessor() {
      static bool trace = false;
      return trace;
    }
  }  // namespace detail

  bool tracing() { return detail::trace_accessor(); }
  void trace_on() { detail::trace_accessor() = true; }
  void trace_off() { detail::trace_accessor() = false; }

  namespace detail {
    inline std::ostream &print_helper(std::ostream &out) { return out; }
    template <typename T, typename... Ts>
    inline std::ostream &print_helper(std::ostream &out, const T &t, const Ts &... ts) {
      out << ' ' << t;
      return print_helper(out, ts...);
    }
    //
    enum class StdOstreamTag { Cout, Cerr };
    template <StdOstreamTag> inline std::mutex &print_mutex_accessor() {
      static std::mutex mutex;
      return mutex;
    }
  }  // namespace detail

  template <typename T, typename... Ts>
  void print(const T &t, const Ts &... ts) {
    std::lock_guard<std::mutex> lock(detail::print_mutex_accessor<detail::StdOstreamTag::Cout>());
    std::cout << t;
    detail::print_helper(std::cout, ts...) << std::endl;
  }

  template <typename T, typename... Ts>
  void print_error(const T &t, const Ts &... ts) {
    std::lock_guard<std::mutex> lock(detail::print_mutex_accessor<detail::StdOstreamTag::Cerr>());
    std::cerr << t;
    detail::print_helper(std::cerr, ts...) << std::endl;
  }

  class OpBase;  // forward decl
  template <typename keyT, typename valueT>
  class In;  // forward decl
  template <typename keyT, typename valueT>
  class Out;  // forward decl

  /// Provides basic information and graph connectivity (eventually statistics,
  /// etc.)
  class TerminalBase {
   public:
    static constexpr bool is_a_terminal = true;

    /// describes the terminal type
    enum class Type {
      Write,   /// can only be written to
      Read,    /// can only be read from
      Consume  /// provides consumable data
    };

   private:
    OpBase *op;                  //< Pointer to containing operation
    size_t n;                    //< Index of terminal
    std::string name;            //< Name of terminal
    std::string key_type_str;    //< String describing key type
    std::string value_type_str;  //< String describing value type

    std::vector<TerminalBase *> successors_;

    TerminalBase(const TerminalBase &) = delete;
    TerminalBase(TerminalBase &&) = delete;

    friend class OpBase;
    template <typename keyT, typename valueT>
    friend class In;
    template <typename keyT, typename valueT>
    friend class Out;

   protected:
    TerminalBase() : op(0), n(0), name("") {}

    void set(OpBase *op, size_t index, const std::string &name, const std::string &key_type_str,
             const std::string &value_type_str, Type type) {
      this->op = op;
      this->n = index;
      this->name = name;
      this->key_type_str = key_type_str;
      this->value_type_str = value_type_str;
    }

    /// Add directed connection (this --> successor) in internal representation of the TTG.
    /// This is called by the derived class's connect method
    void connect_base(TerminalBase *successor) { successors_.push_back(successor); }

    const std::vector<TerminalBase *> &successors() const { return successors_; }

   public:
    /// Return ptr to containing op
    OpBase *get_op() const {
      if (!op) throw "ttg::TerminalBase:get_op() but op is null";
      return op;
    }

    /// Returns index of terminal
    size_t get_index() const {
      if (!op) throw "ttg::TerminalBase:get_index() but op is null";
      return n;
    }

    /// Returns name of terminal
    const std::string &get_name() const {
      if (!op) throw "ttg::TerminalBase:get_name() but op is null";
      return name;
    }

    /// Returns string representation of key type
    const std::string &get_key_type_str() const {
      if (!op) throw "ttg::TerminalBase:get_key_type_str() but op is null";
      return key_type_str;
    }

    /// Returns string representation of value type
    const std::string &get_value_type_str() const {
      if (!op) throw "ttg::TerminalBase:get_value_type_str() but op is null";
      return value_type_str;
    }

    /// Returns the terminal type
    virtual Type get_type() const = 0;

    /// Get connections to successors
    const std::vector<TerminalBase *> &get_connections() const { return successors_; }

    /// Connect this (a TTG output terminal) to a TTG input terminal.
    /// The base class method forwards to the the derived class connect method and so
    /// type checking for the key/value will be done at runtime when performing the
    /// dynamic down cast from TerminalBase* to In<keyT,valueT>.
    virtual void connect(TerminalBase *in) = 0;

    virtual ~TerminalBase() {}
  };

  /// Provides basic information and graph connectivity (eventually statistics,
  /// etc.)
  class OpBase {
   private:
    uint64_t instance_id;  //< Unique ID for object
    static bool trace;     //< If true prints trace of all assignments and all op invocations

    std::string name;
    std::vector<TerminalBase *> inputs;
    std::vector<TerminalBase *> outputs;
    bool trace_instance;              //< If true traces just this instance
    bool is_composite;                //< True if the operator is composite
    bool is_within_composite;         //< True if the operator is part of a composite
    OpBase *containing_composite_op;  //< If part of a composite, points to composite operator

    bool executable;

    // Default copy/move/assign all OK
    static uint64_t next_instance_id() {
      static uint64_t id = 0;
      return id++;
    }

   protected:
    void set_input(size_t i, TerminalBase *t) {
      if (i >= inputs.size()) throw("out of range i setting input");
      inputs[i] = t;
    }

    void set_output(size_t i, TerminalBase *t) {
      if (i >= outputs.size()) throw("out of range i setting output");
      outputs[i] = t;
    }

    template <bool out, typename terminalT, std::size_t i, typename setfuncT>
    void register_terminal(terminalT &term, const std::string &name, const setfuncT setfunc) {
      term.set(this, i, name, detail::demangled_type_name<typename terminalT::key_type>(),
               detail::demangled_type_name<typename terminalT::value_type>(),
               out ? TerminalBase::Type::Write
                   : (std::is_const<typename terminalT::value_type>::value ? TerminalBase::Type::Read
                                                                           : TerminalBase::Type::Consume));
      (this->*setfunc)(i, &term);
    }

    template <bool out, std::size_t... IS, typename terminalsT, typename namesT, typename setfuncT>
    void register_terminals(std::index_sequence<IS...>, terminalsT &terms, const namesT &names,
                            const setfuncT setfunc) {
      int junk[] = {0, (register_terminal<out, typename std::tuple_element<IS, terminalsT>::type, IS>(
                            std::get<IS>(terms), names[IS], setfunc),
                        0)...};
      junk[0]++;
    }

    // Used by op ... terminalsT will be a tuple of terminals
    template <typename terminalsT, typename namesT>
    void register_input_terminals(terminalsT &terms, const namesT &names) {
      register_terminals<false>(std::make_index_sequence<std::tuple_size<terminalsT>::value>{}, terms, names,
                                &OpBase::set_input);
    }

    // Used by op ... terminalsT will be a tuple of terminals
    template <typename terminalsT, typename namesT>
    void register_output_terminals(terminalsT &terms, const namesT &names) {
      register_terminals<true>(std::make_index_sequence<std::tuple_size<terminalsT>::value>{}, terms, names,
                               &OpBase::set_output);
    }

    // Used by composite op ... terminalsT will be a tuple of pointers to terminals
    template <std::size_t... IS, typename terminalsT, typename setfuncT>
    void set_terminals(std::index_sequence<IS...>, terminalsT &terms, const setfuncT setfunc) {
      int junk[] = {0, ((this->*setfunc)(IS, std::get<IS>(terms)), 0)...};
      junk[0]++;
    }

    // Used by composite op ... terminalsT will be a tuple of pointers to terminals
    template <typename terminalsT, typename setfuncT>
    void set_terminals(const terminalsT &terms, const setfuncT setfunc) {
      set_terminals(std::make_index_sequence<std::tuple_size<terminalsT>::value>{}, terms, setfunc);
    }

   public:
    OpBase(const std::string &name, size_t numins, size_t numouts)
        : instance_id(next_instance_id())
        , name(name)
        , inputs(numins)
        , outputs(numouts)
        , trace_instance(false)
        , is_composite(false)
        , is_within_composite(false)
        , containing_composite_op(0)
        , executable(false) {
        //std::cout << name << "@" << (void *)this << " -> " << instance_id << std::endl;
    }

    /// Sets trace for all operations to value and returns previous setting
    static bool set_trace_all(bool value) {
      std::swap(trace, value);
      return value;
    }

    /// Sets trace for just this instance to value and returns previous setting
    bool set_trace_instance(bool value) {
      std::swap(trace_instance, value);
      return value;
    }

    /// Returns true if tracing set for either this instance or all instances
    bool get_trace() { return trace || trace_instance; }
    bool tracing() { return get_trace(); }

    void set_is_composite(bool value) { is_composite = value; }
    bool get_is_composite() const { return is_composite; }
    void set_is_within_composite(bool value, OpBase *op) {
      is_within_composite = value;
      containing_composite_op = op;
    }
    bool get_is_within_composite() const { return is_within_composite; }
    OpBase *get_containing_composite_op() const { return containing_composite_op; }

    /// Sets the name of this operation
    void set_name(const std::string &name) { this->name = name; }

    /// Gets the name of this operation
    const std::string &get_name() const { return name; }

    /// Returns the vector of input terminals
    const std::vector<TerminalBase *> &get_inputs() const { return inputs; }

    /// Returns the vector of output terminals
    const std::vector<TerminalBase *> &get_outputs() const { return outputs; }

    /// Returns a pointer to the i'th input terminal
    TerminalBase *in(size_t i) {
      if (i >= inputs.size()) throw "opbase: you are requesting an input terminal that does not exist";
      return inputs[i];
    }

    /// Returns a pointer to the i'th output terminal
    TerminalBase *out(size_t i) {
      if (i >= outputs.size()) throw "opbase: you are requesting an output terminal that does not exist";
      return outputs[i];
    }

    /// Returns a pointer to the i'th input terminal ... to make API consistent with Op
    template <std::size_t i>
    TerminalBase *in() {
      return in(i);
    }

    /// Returns a pointer to the i'th output terminal ... to make API consistent with Op
    template <std::size_t i>
    TerminalBase *out() {
      return out(i);
    }

    uint64_t get_instance_id() const { return instance_id; }

    /// Waits for the entire TTG associated with this op to be completed (collective)
    virtual void fence() = 0;

    /// Queries if this ready to execute
    /// @return true is this object is executable
    bool is_executable() const { return executable; }

    /// Marks this executable
    /// @return nothing
    virtual void make_executable() = 0;

    virtual ~OpBase() {}
  };

  // With more than one source file this will need to be moved
  bool OpBase::trace = false;

  void OpBase::make_executable() {
    executable = true;
  }


template <typename input_terminalsT, typename output_terminalsT>
  class CompositeOp : public OpBase {
   public:
    static constexpr int numins = std::tuple_size<input_terminalsT>::value;    // number of input arguments
    static constexpr int numouts = std::tuple_size<output_terminalsT>::value;  // number of outputs or results

    using input_terminals_type = input_terminalsT;    // should be a tuple of pointers to input terminals
    using output_terminals_type = output_terminalsT;  // should be a tuple of pointers to output terminals

   private:
    std::vector<std::unique_ptr<OpBase>> ops;
    input_terminals_type ins;
    output_terminals_type outs;

    CompositeOp(const CompositeOp &) = delete;
    CompositeOp &operator=(const CompositeOp &) = delete;
    CompositeOp(const CompositeOp &&) = delete;  // Move should be OK

   public:
    template <typename opsT>
    CompositeOp(opsT &&ops_take_ownership,
                const input_terminals_type &ins,    // tuple of pointers to input terminals
                const output_terminals_type &outs,  // tuple of pointers to output terminals
                const std::string &name = "compositeop")
        : OpBase(name, numins, numouts), ops(std::forward<opsT>(ops_take_ownership)), ins(ins), outs(outs) {
      if (ops.size() == 0) throw "CompositeOp: need to wrap at least one op";  // see fence

      set_is_composite(true);
      for (auto &op : ops) op->set_is_within_composite(true, this);
      set_terminals(ins, &CompositeOp<input_terminalsT, output_terminalsT>::set_input);
      set_terminals(outs, &CompositeOp<input_terminalsT, output_terminalsT>::set_output);

      // traversal is still broken ... need to add checking for composite
    }

    /// Return a pointer to i'th input terminal
    template <std::size_t i>
    typename std::tuple_element<i, input_terminals_type>::type in() {
      return std::get<i>(ins);
    }

    /// Return a pointer to i'th output terminal
    template <std::size_t i>
    typename std::tuple_element<i, output_terminalsT>::type out() {
      return std::get<i>(outs);
    }

    OpBase *get_op(std::size_t i) { return ops.at(i).get(); }

    void fence() { ops[0]->fence(); }

    void make_executable() { for(auto &op : ops) op->make_executable(); }
  };

  template <typename opsT, typename input_terminalsT, typename output_terminalsT>
  std::unique_ptr<CompositeOp<input_terminalsT, output_terminalsT>> make_composite_op(
      opsT &&ops, const input_terminalsT &ins, const output_terminalsT &outs, const std::string &name = "compositeop") {
    return std::make_unique<CompositeOp<input_terminalsT, output_terminalsT>>(std::forward<opsT>(ops), ins, outs, name);
  }

  namespace detail {
  /// Traverses a graph of ops in depth-first manner following out edges
  class Traverse {
    std::set<OpBase *> seen;

    bool visited(OpBase *p) { return !seen.insert(p).second; }

   public:
    virtual void opfunc(OpBase *op) = 0;

    virtual void infunc(TerminalBase *in) = 0;

    virtual void outfunc(TerminalBase *out) = 0;

    void reset() { seen.clear(); }

    // Returns true if no null pointers encountered (i.e., if all
    // encountered terminals/operations are connected)
    bool traverse(OpBase *op) {
      if (!op) {
        std::cout << "ttg::Traverse: got a null op!\n";
        return false;
      }

      if (visited(op)) return true;

      bool status = true;

      opfunc(op);

      for (auto in : op->get_inputs()) {
        if (!in) {
          std::cout << "ttg::Traverse: got a null in!\n";
          status = false;
        } else {
          infunc(in);
        }
      }

      for (auto out : op->get_outputs()) {
        if (!out) {
          std::cout << "ttg::Traverse: got a null out!\n";
          status = false;
        } else {
          outfunc(out);
        }
      }

      for (auto out : op->get_outputs()) {
        if (out) {
          for (auto successor : out->get_connections()) {
            if (!successor) {
              std::cout << "ttg::Traverse: got a null successor!\n";
              status = false;
            } else {
              status = status && traverse(successor->get_op());
            }
          }
        }
      }

      return status;
    }
  };
  }

  /// @brief Traverses a graph of ops in depth-first manner following out edges
  /// @tparam OpVisitor A Callable type that visits each Op
  /// @tparam InVisitor A Callable type that visits each In terminal
  /// @tparam OutVisitor A Callable type that visits each Out terminal
  template <typename OpVisitor, typename InVisitor, typename OutVisitor>
  class Traverse : private detail::Traverse {
   public:
    static_assert(std::is_void<::ttg::meta::void_t<decltype(std::declval<OpVisitor>()(std::declval<OpBase *>()))>>::value,
                  "Traverse<OpVisitor,...>: OpVisitor(const OpBase *op) must be a valid expression");
    static_assert(std::is_void<::ttg::meta::void_t<decltype(std::declval<InVisitor>()(std::declval<TerminalBase *>()))>>::value,
                  "Traverse<,InVisitor,>: InVisitor(const TerminalBase *op) must be a valid expression");
    static_assert(std::is_void<::ttg::meta::void_t<decltype(std::declval<OutVisitor>()(std::declval<TerminalBase *>()))>>::value,
                  "Traverse<...,OutVisitor>: OutVisitor(const TerminalBase *op) must be a valid expression");

    template <typename OpVisitor_, typename InVisitor_, typename OutVisitor_>
    Traverse(OpVisitor_&& op_v, InVisitor_&& in_v, OutVisitor_&& out_v) :
        op_visitor_(std::forward<OpVisitor_>(op_v)),
        in_visitor_(std::forward<InVisitor_>(in_v)),
        out_visitor_(std::forward<OutVisitor_>(out_v))
    {};

    const  OpVisitor&  op_visitor() const { return  op_visitor_; }
    const  InVisitor&  in_visitor() const { return  in_visitor_; }
    const OutVisitor& out_visitor() const { return out_visitor_; }

    bool operator()(OpBase* op) {
      reset();
      const bool result = traverse(op);
      reset();
      return result;
    }

   private:
    OpVisitor op_visitor_;
    InVisitor in_visitor_;
    OutVisitor out_visitor_;

    void opfunc(OpBase *op) { op_visitor_(op); }

    void infunc(TerminalBase *in) { in_visitor_(in); }

    void outfunc(TerminalBase *out) { out_visitor_(out); }
  };

  template <typename OpVisitor, typename InVisitor, typename OutVisitor>
  auto make_traverse(OpVisitor&& op_v, InVisitor&& in_v, OutVisitor&& out_v) {
    return Traverse<std::remove_reference_t<OpVisitor>,
                    std::remove_reference_t<InVisitor>,
                    std::remove_reference_t<OutVisitor>>{
        std::forward<OpVisitor>(op_v),
        std::forward<InVisitor>(in_v),
        std::forward<OutVisitor>(out_v)
    };
  };

  /// @brief Verifies graph connectivity
  class Verify : private detail::Traverse {
    void opfunc(OpBase *op) {}
    void infunc(TerminalBase *in) {}
    void outfunc(TerminalBase *out) {}

   public:
    /// Traverses graph starting at @c op
    /// @return true if traversal from this Op does not reveal dangling (non-connected) Out terminals
    bool operator()(const OpBase *op) {
      reset();
      bool status = traverse(const_cast<OpBase*>(op));
      reset();
      return status;
    }
  };

  /// Prints the graph to std::cout in an ad hoc format
  class Print : private detail::Traverse {
    void opfunc(OpBase *op) {
      std::cout << "op: " << (void *)op << " " << op->get_name() << " numin " << op->get_inputs().size() << " numout "
                << op->get_outputs().size() << std::endl;
    }

    void infunc(TerminalBase *in) {
      std::cout << "  in: " << in->get_index() << " " << in->get_name() << " " << in->get_key_type_str() << " "
                << in->get_value_type_str() << std::endl;
    }

    void outfunc(TerminalBase *out) {
      std::cout << " out: " << out->get_index() << " " << out->get_name() << " " << out->get_key_type_str() << " "
                << out->get_value_type_str() << std::endl;
    }

   public:
    /// @return true if traversal from this Op does not reveal dangling (non-connected) Out terminals
    bool operator()(const OpBase *op) {
      reset();
      bool status = traverse(const_cast<OpBase*>(op));
      reset();
      return status;
    }
  };

  /// Prints the graph to a std::string in the format understood by GraphViz's dot program
  class Dot : private detail::Traverse {
    std::stringstream buf;

    // Insert backslash before characters that dot is interpreting
    std::string escape(const std::string &in) {
      std::stringstream s;
      for (char c : in) {
        if (c == '<' || c == '>' || c == '"')
          s << "\\" << c;
        else
          s << c;
      }
      return s.str();
    }

    // A unique name for the node derived from the pointer
    std::string nodename(const OpBase *op) {
      std::stringstream s;
      s << "n" << (void *)op;
      return s.str();
    }

    void opfunc(OpBase *op) {
      std::string opnm = nodename(op);

      buf << "        " << opnm << " [shape=record,style=filled,fillcolor=gray90,label=\"{";

      size_t count = 0;
      if (op->get_inputs().size() > 0) buf << "{";
      for (auto in : op->get_inputs()) {
        if (in) {
          if (count != in->get_index()) throw "ttg::Dot: lost count of ins";
          buf << " <in" << count << ">"
              << " " << escape("<" + in->get_key_type_str() + "," + in->get_value_type_str() + ">") << " "
              << in->get_name();
        } else {
          buf << " <in" << count << ">"
              << " unknown ";
        }
        count++;
        if (count < op->get_inputs().size()) buf << " |";
      }
      if (op->get_inputs().size() > 0) buf << "} |";

      buf << op->get_name() << " ";

      if (op->get_outputs().size() > 0) buf << " | {";

      count = 0;
      for (auto out : op->get_outputs()) {
        if (out) {
          if (count != out->get_index()) throw "ttg::Dot: lost count of outs";
          buf << " <out" << count << ">"
              << " " << escape("<" + out->get_key_type_str() + "," + out->get_value_type_str() + ">") << " "
              << out->get_name();
        } else {
          buf << " <out" << count << ">"
              << " unknown ";
        }
        count++;
        if (count < op->get_outputs().size()) buf << " |";
      }

      if (op->get_outputs().size() > 0) buf << "}";

      buf << " } \"];\n";

      for (auto out : op->get_outputs()) {
        if (out) {
          for (auto successor : out->get_connections()) {
            if (successor) {
              buf << opnm << ":out" << out->get_index() << ":s -> " << nodename(successor->get_op()) << ":in"
                  << successor->get_index() << ":n;\n";
            }
          }
        }
      }
    }

    void infunc(TerminalBase *in) {}

    void outfunc(TerminalBase *out) {}

   public:
    /// @return string containing the graph specification in the format understood by GraphViz's dot program
    std::string operator()(const OpBase *op) {
      reset();
      buf.str(std::string());
      buf.clear();

      buf << "digraph G {\n";
      buf << "        ranksep=1.5;\n";
      traverse(const_cast<OpBase*>(op));
      buf << "}\n";

      reset();
      std::string result = buf.str();
      buf.str(std::string());
      buf.clear();

      return result;
    }
  };

  /// applies @c make_executable method to every op in the graph
  /// return true if there are no dangling out terminals
  bool make_graph_executable(OpBase* op) {
    return ::ttg::make_traverse(
        [](auto x) { x->make_executable(); },
        [](auto x) {},
        [](auto x) {}
    )(op);
  }


template <typename keyT, typename valueT>
  class Edge;  // Forward decl.

  template <typename keyT, typename valueT>
  class In : public TerminalBase {
   public:
    typedef valueT value_type;
    typedef keyT key_type;
    static_assert(std::is_same<keyT, std::decay_t<keyT>>::value,
                  "In<keyT,valueT> assumes keyT is a non-decayable type");
    // valueT can be T or const T
    static_assert(std::is_same<std::remove_const_t<valueT>, std::decay_t<valueT>>::value,
                  "In<keyT,valueT> assumes std::remove_const<T> is a non-decayable type");
    typedef Edge<keyT, valueT> edge_type;
    using send_callback_type = std::function<void(const keyT &, const std::decay_t<valueT> &)>;
    using move_callback_type = std::function<void(const keyT &, std::decay_t<valueT> &&)>;
    static constexpr bool is_an_input_terminal = true;

   private:
    bool initialized;
    send_callback_type send_callback;
    move_callback_type move_callback;

    // No moving, copying, assigning permitted
    In(In &&other) = delete;
    In(const In &other) = delete;
    In &operator=(const In &other) = delete;
    In &operator=(const In &&other) = delete;

    void connect(TerminalBase *p) override {
      throw "to connect terminals use out->connect(in) rather than in->connect(out)";
    }

   public:
    In() : initialized(false) {}

    In(const send_callback_type &send_callback, const move_callback_type &move_callback)
        : initialized(true), send_callback(send_callback), move_callback(move_callback) {}

    // callback (std::function) is used to erase the operator type and argument
    // index
    void set_callback(const send_callback_type &send_callback, const move_callback_type &move_callback) {
      initialized = true;
      this->send_callback = send_callback;
      this->move_callback = move_callback;
    }

    void send(const keyT &key, const valueT &value) {
      // std::cout << "In::send-constref::\n";
      if (!initialized) throw "sending to uninitialzed callback";
      send_callback(key, value);
    }

    void send(const keyT &key, valueT &&value) {
      // std::cout << "In::send-move::\n";
      if (!initialized) throw "sending to uninitialzed callback";
      move_callback(key, std::forward<valueT>(value));
    }

    // An optimized implementation will need a separate callback for broadcast
    // with a specific value for rangeT
    template <typename rangeT>
    void broadcast(const rangeT &keylist, const valueT &value) {
      if (!initialized) throw "broadcasting to uninitialzed callback";
      for (auto key : keylist) send(key, value);
    }

    Type get_type() const override {
      return std::is_const<valueT>::value ? TerminalBase::Type::Read : TerminalBase::Type::Consume;
    }
  };

  // Output terminal
  template <typename keyT, typename valueT>
  class Out : public TerminalBase {
   public:
    typedef valueT value_type;
    typedef keyT key_type;
    static_assert(std::is_same<keyT, std::decay_t<keyT>>::value,
                  "Out<keyT,valueT> assumes keyT is a non-decayable type");
    static_assert(std::is_same<valueT, std::decay_t<valueT>>::value,
                  "Out<keyT,valueT> assumes valueT is a non-decayable type");
    typedef Edge<keyT, valueT> edge_type;
    static constexpr bool is_an_output_terminal = true;

   private:
    // No moving, copying, assigning permitted
    Out(Out &&other) = delete;
    Out(const Out &other) = delete;
    Out &operator=(const Out &other) = delete;
    Out &operator=(const Out &&other) = delete;

   public:
    Out() {}

    /// \note will check data types unless macro \c NDEBUG is defined
    void connect(TerminalBase *in) override {
#ifndef NDEBUG
      if (in->get_type() == TerminalBase::Type::Read) {
        typedef In<keyT, std::add_const_t<valueT>> input_terminal_type;
        if (!dynamic_cast<input_terminal_type *>(in))
          throw std::invalid_argument(
              std::string("you are trying to connect terminals with incompatible types:\ntype of this Terminal = ") +
              detail::demangled_type_name(this) + "\ntype of other Terminal" + detail::demangled_type_name(in));
      } else if (in->get_type() == TerminalBase::Type::Consume) {
        typedef In<keyT, valueT> input_terminal_type;
        if (!dynamic_cast<input_terminal_type *>(in))
          throw std::invalid_argument(
              std::string("you are trying to connect terminals with incompatible types:\ntype of this Terminal = ") +
              detail::demangled_type_name(this) + "\ntype of other Terminal" + detail::demangled_type_name(in));
      } else  // successor->type() == TerminalBase::Type::Write
        throw std::invalid_argument(std::string("you are trying to connect an Out terminal to another Out terminal"));
#endif
      this->connect_base(in);
    }

    void send(const keyT &key, const valueT &value) {
      for (auto successor : successors()) {
        assert(successor->get_type() != TerminalBase::Type::Write);
        if (successor->get_type() == TerminalBase::Type::Read) {
          static_cast<In<keyT, std::add_const_t<valueT>> *>(successor)->send(key, value);
        } else if (successor->get_type() == TerminalBase::Type::Consume) {
          static_cast<In<keyT, valueT> *>(successor)->send(key, value);
        }
      }
    }

    void send(const keyT &key, valueT &&value) {
      std::size_t N = successors().size();
      // find the first terminal that can consume the value
      std::size_t move_terminal = N - 1;
      for (std::size_t i = 0; i != N; ++i) {
        if (successors()[i]->get_type() == TerminalBase::Type::Consume) {
          move_terminal = i;
          break;
        }
      }
      if (N > 0) {
        // send copies to every terminal except the one we will move the results to
        for (std::size_t i = 0; i != N; ++i) {
          if (i != move_terminal) {
            TerminalBase *successor = successors()[i];
            if (successor->get_type() == TerminalBase::Type::Read) {
              static_cast<In<keyT, std::add_const_t<valueT>> *>(successor)->send(key, value);
            } else if (successor->get_type() == TerminalBase::Type::Consume) {
              static_cast<In<keyT, valueT> *>(successor)->send(key, value);
            }
          }
        }
        {
          TerminalBase *successor = successors()[move_terminal];
          static_cast<In<keyT, valueT> *>(successor)->send(key, std::forward<valueT>(value));
        }
      }
    }

    // An optimized implementation will need a separate callback for broadcast
    // with a specific value for rangeT
    template <typename rangeT>
    void broadcast(const rangeT &keylist, const valueT &value) {  // NO MOVE YET
      for (auto successor : successors()) {
        assert(successor->get_type() != TerminalBase::Type::Write);
        if (successor->get_type() == TerminalBase::Type::Read) {
          static_cast<In<keyT, std::add_const_t<valueT>> *>(successor)->broadcast(keylist, value);
        } else if (successor->get_type() == TerminalBase::Type::Consume) {
          static_cast<In<keyT, valueT> *>(successor)->broadcast(keylist, value);
        }
      }
    }

    Type get_type() const override { return TerminalBase::Type::Write; }
  };

  template <typename out_terminalT, typename in_terminalT>
  void connect(out_terminalT *out, in_terminalT *in) {
    out->connect(in);
  }

  // This should match unique ptrs
  template <std::size_t outindex, std::size_t inindex, typename producer_op_ptr, typename successor_op_ptr>
  void connect(producer_op_ptr &p, successor_op_ptr &s) {
    connect(p->template out<outindex>(), s->template in<inindex>());
  }

  // This should match bare ptrs
  template <std::size_t outindex, std::size_t inindex, typename producer_op_ptr, typename successor_op_ptr>
  void connect(producer_op_ptr *p, successor_op_ptr *s) {
    connect(p->template out<outindex>(), s->template in<inindex>());
  }

  template <typename keyT, typename valueT>
  class Edge {
   private:
    // An EdgeImpl represents a single edge that most usually will
    // connect a single output terminal with a single
    // input terminal.  However, we had to relax this constraint in
    // order to easily accommodate connecting an input/output edge to
    // an operation that to the outside looked like a single op but
    // internally was implemented as multiple operations.  Thus, the
    // input/output edge has to connect to multiple terminals.
    // Permitting multiple end points makes this much easier to
    // compose, easier to implement, and likely more efficient at
    // runtime.  This is why outs/ins are vectors rather than pointers
    // to a single terminal.
    struct EdgeImpl {
      std::string name;
      std::vector<TerminalBase *> outs;  // In<keyT, valueT> or In<keyT, const valueT>
      std::vector<Out<keyT, valueT> *> ins;

      EdgeImpl() : name(""), outs(), ins() {}

      EdgeImpl(const std::string &name) : name(name), outs(), ins() {}

      void set_in(Out<keyT, valueT> *in) {
        if (ins.size() && tracing()) std::cout << "Edge: " << name << " : has multiple inputs" << std::endl;
        ins.push_back(in);
        try_to_connect_new_in(in);
      }

      void set_out(TerminalBase *out) {
        if (outs.size() && tracing()) std::cout << "Edge: " << name << " : has multiple outputs" << std::endl;
        outs.push_back(out);
        try_to_connect_new_out(out);
      }

      void try_to_connect_new_in(Out<keyT, valueT> *in) const {
        for (auto out : outs)
          if (in && out) in->connect(out);
      }

      void try_to_connect_new_out(TerminalBase *out) const {
        assert(out->get_type() != TerminalBase::Type::Write);  // out must be an In<>
        for (auto in : ins)
          if (in && out) in->connect(out);
      }

      ~EdgeImpl() {
        if (ins.size() == 0 || outs.size() == 0) {
          std::cerr << "Edge: destroying edge pimpl with either in or out not "
                       "assigned --- graph may be incomplete"
                    << std::endl;
        }
      }
    };

    // We have a vector here to accomodate fusing multiple edges together
    // when connecting them all to a single terminal.
    mutable std::vector<std::shared_ptr<EdgeImpl>> p;  // Need shallow copy semantics

   public:
    typedef Out<keyT, valueT> output_terminal_type;
    typedef keyT key_type;
    typedef valueT value_type;
    static_assert(std::is_same<keyT, std::decay_t<keyT>>::value,
                  "Edge<keyT,valueT> assumes keyT is a non-decayable type");
    static_assert(std::is_same<valueT, std::decay_t<valueT>>::value,
                  "Edge<keyT,valueT> assumes valueT is a non-decayable type");
    static constexpr bool is_an_edge = true;

    Edge(const std::string name = "anonymous edge") : p(1) { p[0] = std::make_shared<EdgeImpl>(name); }

    template <typename... valuesT>
    Edge(const Edge<keyT, valuesT> &... edges) : p(0) {
      std::vector<Edge<keyT, valueT>> v = {edges...};
      for (auto &edge : v) {
        p.insert(p.end(), edge.p.begin(), edge.p.end());
      }
    }

    void set_in(Out<keyT, valueT> *in) const {
      for (auto &edge : p) edge->set_in(in);
    }

    void set_out(TerminalBase *out) const {
      for (auto &edge : p) edge->set_out(out);
    }

    // this is currently just a hack, need to understand better whether this is a good idea
    Out<keyT, valueT> *in(size_t edge_index = 0, size_t terminal_index = 0) {
      return p.at(edge_index)->ins.at(terminal_index);
    }
  };

  // Fuse edges into one ... all the types have to be the same ... just using
  // valuesT for variadic args
  template <typename keyT, typename... valuesT>
  auto fuse(const Edge<keyT, valuesT> &... args) {
    using valueT = typename std::tuple_element<0, std::tuple<valuesT...>>::type;  // grab first type
    return Edge<keyT, valueT>(args...);  // This will force all valuesT to be the same
  }

  // Make a tuple of Edges ... needs some type checking injected
  template <typename... inedgesT>
  auto edges(const inedgesT &... args) {
    return std::make_tuple(args...);
  }

  // template <typename keyT, typename valueT,
  //           typename output_terminalT>
  // void send(const keyT& key, valueT& value,
  //           output_terminalT& t) {
  //     t.send(key, value);
  // }

  // template <size_t i, typename keyT, typename valueT,
  //           typename... output_terminalsT>
  // void send(const keyT& key, valueT& value,
  //           std::tuple<output_terminalsT...>& t) {
  //     std::get<i>(t).send(key, value);
  // }

  template <typename keyT, typename valueT, typename output_terminalT>
  void send(const keyT &key, valueT &&value, output_terminalT &t) {
    // std::cout << "::send move\n";
    t.send(key, std::forward<valueT>(value));
  }

  template <size_t i, typename keyT, typename valueT, typename... output_terminalsT>
  void send(const keyT &key, valueT &&value, std::tuple<output_terminalsT...> &t) {
    // std::cout << "::send<> move\n";
    std::get<i>(t).send(key, std::forward<valueT>(value));
  }

  template <size_t i, typename rangeT, typename valueT, typename... output_terminalsT>
  void broadcast(const rangeT &keylist, valueT &&value, std::tuple<output_terminalsT...> &t) {
    std::get<i>(t).broadcast(keylist, std::forward<valueT>(value));
  }

  // Make type of tuple of edges from type of tuple of terminals
  template <typename termsT>
  struct terminals_to_edges;
  template <typename... termsT>
  struct terminals_to_edges<std::tuple<termsT...>> {
    typedef std::tuple<typename termsT::edge_type...> type;
  };

  // Make type of tuple of output terminals from type of tuple of edges
  template <typename edgesT>
  struct edges_to_output_terminals;
  template <typename... edgesT>
  struct edges_to_output_terminals<std::tuple<edgesT...>> {
    typedef std::tuple<typename edgesT::output_terminal_type...> type;
  };

}  // namespace ttg

// This provides an efficent API for serializing/deserializing a data type.
// An object of this type will need to be provided for each serializable type.
// The default implementation, in serialization.h, works only for primitive/POD data types;
// backend-specific implementations may be available in backend/serialization.h .
extern "C" struct ttg_data_descriptor {
  const char *name;
  void (*get_info)(const void *object, uint64_t *hs, uint64_t *ps, int *is_contiguous_mask, void **buf);
  void (*pack_header)(const void *object, uint64_t header_size, void **buf);
  void (*pack_payload)(const void *object, uint64_t *chunk_size, uint64_t pos, void **buf);
  void (*unpack_header)(void *object, uint64_t header_size, const void *buf);
  void (*unpack_payload)(void *object, uint64_t chunk_size, uint64_t pos, const void *buf);
  void (*print)(const void *object);
};

namespace ttg {

  template <typename T, typename Enabler>
  struct default_data_descriptor;

  // Returns a pointer to a constant static instance initialized
  // once at run time.
  template <typename T>
  const ttg_data_descriptor *get_data_descriptor();

}  // namespace ttg

#endif  // TTG_H_INCLUDED
