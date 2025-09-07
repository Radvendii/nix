#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"

#include <sstream>

#include <toml.hpp>

namespace nix {

static void prim_fromTOML(EvalState & state, const PosIdx pos, ValueRef * args, ValueRef val)
{
    auto toml = state.forceStringNoCtx(args[0], pos, "while evaluating the argument passed to builtins.fromTOML");

    std::istringstream tomlStream(std::string{toml});

    std::function<void(ValueRef, toml::value)> visit;

    visit = [&](ValueRef v, toml::value t) {
        switch (t.type()) {
        case toml::value_t::table: {
            auto table = toml::get<toml::table>(t);

            size_t size = 0;
            for (auto & i : table) {
                (void) i;
                size++;
            }

            auto attrs = state.buildBindings(size);

            for (auto & elem : table) {
                forceNoNullByte(elem.first);
                visit(attrs.alloc(elem.first), elem.second);
            }

            v.mkAttrs(state.values, attrs);
        } break;
            ;
        case toml::value_t::array: {
            auto array = toml::get<std::vector<toml::value>>(t);

            auto list = state.buildList(array.size());
            for (const auto & [n, v] : enumerate(list))
                visit(v = state.allocValue(), array[n]);
            v.mkList(state.values, list);
        } break;
            ;
        case toml::value_t::boolean:
            v.mkBool(state.values, toml::get<bool>(t));
            break;
            ;
        case toml::value_t::integer:
            v.mkInt(state.values, toml::get<int64_t>(t));
            break;
            ;
        case toml::value_t::floating:
            v.mkFloat(state.values, toml::get<NixFloat>(t));
            break;
            ;
        case toml::value_t::string: {
            auto s = toml::get<std::string_view>(t);
            forceNoNullByte(s);
            v.mkString(state.values, s);
        } break;
            ;
        case toml::value_t::local_datetime:
        case toml::value_t::offset_datetime:
        case toml::value_t::local_date:
        case toml::value_t::local_time: {
            if (experimentalFeatureSettings.isEnabled(Xp::ParseTomlTimestamps)) {
                auto attrs = state.buildBindings(2);
                attrs.alloc("_type").mkString(state.values, "timestamp");
                std::ostringstream s;
                s << t;
                auto str = toView(s);
                forceNoNullByte(str);
                attrs.alloc("value").mkString(state.values, str);
                v.mkAttrs(state.values, attrs);
            } else {
                throw std::runtime_error("Dates and times are not supported");
            }
        } break;
            ;
        case toml::value_t::empty:
            v.mkNull(state.values);
            break;
            ;
        }
    };

    try {
        visit((val), toml::parse(tomlStream, "fromTOML" /* the "filename" */));
    } catch (std::exception & e) { // TODO: toml::syntax_error
        state.error<EvalError>("while parsing TOML: %s", e.what()).atPos(pos).debugThrow();
    }
}

static RegisterPrimOp primop_fromTOML(
    {.name = "fromTOML",
     .args = {"e"},
     .doc = R"(
      Convert a TOML string to a Nix value. For example,

      ```nix
      builtins.fromTOML ''
        x=1
        s="a"
        [table]
        y=2
      ''
      ```

      returns the value `{ s = "a"; table = { y = 2; }; x = 1; }`.
    )",
     .fun = prim_fromTOML});

} // namespace nix
