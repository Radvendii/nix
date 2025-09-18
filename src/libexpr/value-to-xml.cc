#include "nix/expr/value-to-xml.hh"
#include "nix/util/xml-writer.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/util/signals.hh"

#include <cstdlib>

namespace nix {

static XMLAttrs singletonAttrs(const std::string & name, std::string_view value)
{
    XMLAttrs attrs;
    attrs[name] = value;
    return attrs;
}

static void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    ValueRef v,
    XMLWriter & doc,
    NixStringContext & context,
    PathSet & drvsSeen,
    const PosIdx pos);

static void posToXML(EvalState & state, XMLAttrs & xmlAttrs, const Pos & pos)
{
    if (auto path = std::get_if<SourcePath>(&pos.origin))
        xmlAttrs["path"] = path->path.abs();
    xmlAttrs["line"] = fmt("%1%", pos.line);
    xmlAttrs["column"] = fmt("%1%", pos.column);
}

static void showAttrs(
    EvalState & state,
    bool strict,
    bool location,
    const Bindings & attrs,
    XMLWriter & doc,
    NixStringContext & context,
    PathSet & drvsSeen)
{
    StringSet names;

    for (auto & a : attrs.lexicographicOrder(state.symbols)) {
        XMLAttrs xmlAttrs;
        xmlAttrs["name"] = state.symbols[a->name];
        if (location && a->pos)
            posToXML(state, xmlAttrs, state.positions[a->pos]);

        XMLOpenElement _(doc, "attr", xmlAttrs);
        printValueAsXML(state, strict, location, a->value, doc, context, drvsSeen, a->pos);
    }
}

static void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    ValueRef v,
    XMLWriter & doc,
    NixStringContext & context,
    PathSet & drvsSeen,
    const PosIdx pos)
{
    checkInterrupt();

    if (strict)
        state.forceValue(v, pos);

    switch (v.type(state.values)) {

    case nInt:
        doc.writeEmptyElement("int", singletonAttrs("value", fmt("%1%", v.integer(state.values))));
        break;

    case nBool:
        doc.writeEmptyElement("bool", singletonAttrs("value", v.boolean(state.values) ? "true" : "false"));
        break;

    case nString:
        /* !!! show the context? */
        copyContext(state, v, context);
        doc.writeEmptyElement("string", singletonAttrs("value", v.c_str(state.values)));
        break;

    case nPath:
        doc.writeEmptyElement("path", singletonAttrs("value", v.path(state.values).to_string()));
        break;

    case nNull:
        doc.writeEmptyElement("null");
        break;

    case nAttrs:
        if (state.isDerivation(v)) {
            XMLAttrs xmlAttrs;

            Path drvPath;
            if (auto a = v.attrs(state.values)->get(state.sDrvPath)) {
                if (strict)
                    state.forceValue(a->value, a->pos);
                if (a->value.type(state.values) == nString)
                    xmlAttrs["drvPath"] = drvPath = a->value.c_str(state.values);
            }

            if (auto a = v.attrs(state.values)->get(state.sOutPath)) {
                if (strict)
                    state.forceValue(a->value, a->pos);
                if (a->value.type(state.values) == nString)
                    xmlAttrs["outPath"] = a->value.c_str(state.values);
            }

            XMLOpenElement _(doc, "derivation", xmlAttrs);

            if (drvPath != "" && drvsSeen.insert(drvPath).second)
                showAttrs(state, strict, location, *v.attrs(state.values), doc, context, drvsSeen);
            else
                doc.writeEmptyElement("repeated");
        }

        else {
            XMLOpenElement _(doc, "attrs");
            showAttrs(state, strict, location, *v.attrs(state.values), doc, context, drvsSeen);
        }

        break;

    case nList: {
        XMLOpenElement _(doc, "list");
        for (auto v2 : v.listView(state.values))
            printValueAsXML(state, strict, location, v2, doc, context, drvsSeen, pos);
        break;
    }

    case nFunction: {
        if (!v.isLambda(state.values)) {
            // FIXME: Serialize primops and primopapps
            doc.writeEmptyElement("unevaluated");
            break;
        }
        XMLAttrs xmlAttrs;
        if (location)
            posToXML(state, xmlAttrs, state.positions[v.lambda(state.values).fun.payload(state.exprs).pos]);
        XMLOpenElement _(doc, "function", xmlAttrs);

        if (v.lambda(state.values).fun.hasFormals(state.exprs)) {
            XMLAttrs attrs;
            if (v.lambda(state.values).fun.payload(state.exprs).arg)
                attrs["name"] = state.symbols[v.lambda(state.values).fun.payload(state.exprs).arg];
            if (v.lambda(state.values).fun.payload(state.exprs).formals->ellipsis)
                attrs["ellipsis"] = "1";
            XMLOpenElement _(doc, "attrspat", attrs);
            for (auto & i : v.lambda(state.values).fun.payload(state.exprs).formals->lexicographicOrder(state.symbols))
                doc.writeEmptyElement("attr", singletonAttrs("name", state.symbols[i.name]));
        } else
            doc.writeEmptyElement("varpat", singletonAttrs("name", state.symbols[v.lambda(state.values).fun.payload(state.exprs).arg]));

        break;
    }

    case nExternal:
        v.external(state.values)->printValueAsXML(state, strict, location, doc, context, drvsSeen, pos);
        break;

    case nFloat:
        doc.writeEmptyElement("float", singletonAttrs("value", fmt("%1%", v.fpoint(state.values))));
        break;

    case nThunk:
        doc.writeEmptyElement("unevaluated");
    }
}

void ExternalValueBase::printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    XMLWriter & doc,
    NixStringContext & context,
    PathSet & drvsSeen,
    const PosIdx pos) const
{
    doc.writeEmptyElement("unevaluated");
}

void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    ValueRef v,
    std::ostream & out,
    NixStringContext & context,
    const PosIdx pos)
{
    XMLWriter doc(true, out);
    XMLOpenElement root(doc, "expr");
    PathSet drvsSeen;
    printValueAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
}

} // namespace nix
