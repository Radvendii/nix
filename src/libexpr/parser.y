%define api.location.type { ::nix::ParserLocation }
%define api.pure
%locations
%define parse.error verbose
%defines
/* %no-lines */
%parse-param { void * scanner }
%parse-param { nix::ParserState * state }
%lex-param { void * scanner }
%lex-param { nix::ParserState * state }
%expect 0

%code requires {

#ifndef BISON_HEADER
#define BISON_HEADER

#include <variant>

#include "nix/util/finally.hh"
#include "nix/util/util.hh"
#include "nix/util/users.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/parser-state.hh"

// Bison seems to have difficulty growing the parser stack when using C++ with
// a custom location type. This undocumented macro tells Bison that our
// location type is "trivially copyable" in C++-ese, so it is safe to use the
// same memcpy macro it uses to grow the stack that it uses with its own
// default location type. Without this, we get "error: memory exhausted" when
// parsing some large Nix files. Our other options are to increase the initial
// stack size (200 by default) to be as large as we ever want to support (so
// that growing the stack is unnecessary), or redefine the stack-relocation
// macro ourselves (which is also undocumented).
#define YYLTYPE_IS_TRIVIAL 1

#define YY_DECL int yylex \
    (YYSTYPE * yylval_param, YYLTYPE * yylloc_param, yyscan_t yyscanner, nix::ParserState * state)

// For efficiency, we only track offsets; not line,column coordinates
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).beginOffset = YYRHSLOC (Rhs, 1).beginOffset;        \
          (Current).endOffset  = YYRHSLOC (Rhs, N).endOffset;           \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).beginOffset = (Current).endOffset =                 \
            YYRHSLOC (Rhs, 0).endOffset;                                \
        }                                                               \
    while (0)

namespace nix {

typedef std::unordered_map<PosIdx, DocComment> DocCommentMap;

Expr * parseExprFromBuf(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    SymbolTable & symbols,
    const EvalSettings & settings,
	Values & values,
	Exprs & exprs,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS,
    const Expr::AstSymbols & astSymbols);

}

#endif

}

%{

#include "parser-tab.hh"
#include "lexer-tab.hh"

YY_DECL;

using namespace nix;

#define CUR_POS state->at(yyloc)


void yyerror(YYLTYPE * loc, yyscan_t scanner, ParserState * state, const char * error)
{
    if (std::string_view(error).starts_with("syntax error, unexpected end of file")) {
        loc->beginOffset = loc->endOffset;
    }
    throw ParseError({
        .msg = HintFmt(error),
        .pos = state->positions[state->at(*loc)]
    });
}

#define SET_DOC_POS(lambda, pos) setDocPosition(state, lambda, state->at(pos))
static void setDocPosition(ParserState * state, ExprLambdaRef lambda, PosIdx start) {
    auto it = state->lexerState.positionToDocComment.find(start);
    if (it != state->lexerState.positionToDocComment.end()) {
        state->exprs.ERtoEP(lambda)->setDocComment(state->exprs, it->second);
    }
}

static ExprRef makeCall(Exprs & exprs, PosIdx pos, ExprRef fn, ExprRef arg) {
    if (auto e2 = dynamic_cast<ExprCall *>(exprs.ERtoEP(fn))) {
        e2->args.push_back(arg);
        return fn;
    }
    return exprs.addExprCall(pos, fn, {arg});
}


%}

%union {
  // !!! We're probably leaking stuff here.
  nix::ExprRef e;
  nix::ExprListRef list;
  nix::ExprAttrsRef attrs;
  nix::Formals * formals;
  nix::Formal * formal;
  nix::NixInt n;
  nix::NixFloat nf;
  nix::StringToken id; // !!! -> Symbol
  nix::StringToken path;
  nix::StringToken uri;
  nix::StringToken str;
  std::vector<nix::AttrName> * attrNames;
  std::vector<std::pair<nix::AttrName, nix::PosIdx>> * inheritAttrs;
  std::vector<std::pair<nix::PosIdx, nix::ExprRef>> * string_parts;
  std::vector<std::pair<nix::PosIdx, std::variant<nix::ExprRef, nix::StringToken>>> * ind_string_parts;
}

%type <e> start expr expr_function expr_if expr_op
%type <e> expr_select expr_simple expr_app
%type <e> expr_pipe_from expr_pipe_into
%type <list> expr_list
%type <attrs> binds binds1
%type <formals> formals formal_set
%type <formal> formal
%type <attrNames> attrpath
%type <inheritAttrs> attrs
%type <string_parts> string_parts_interpolated
%type <ind_string_parts> ind_string_parts
%type <e> path_start string_parts string_attr
%type <id> attr
%token <id> ID
%token <str> STR IND_STR
%token <n> INT_LIT
%token <nf> FLOAT_LIT
%token <path> PATH HPATH SPATH PATH_END
%token <uri> URI
%token IF THEN ELSE ASSERT WITH LET IN_KW REC INHERIT EQ NEQ AND OR IMPL OR_KW
%token PIPE_FROM PIPE_INTO /* <| and |> */
%token DOLLAR_CURLY /* == ${ */
%token IND_STRING_OPEN IND_STRING_CLOSE
%token ELLIPSIS


%right IMPL
%left OR
%left AND
%nonassoc EQ NEQ
%nonassoc '<' '>' LEQ GEQ
%right UPDATE
%left NOT
%left '+' '-'
%left '*' '/'
%right CONCAT
%nonassoc '?'
%nonassoc NEGATE

%%

start: expr {
  state->result = state->exprs.ERtoEP($1);

  // This parser does not use yynerrs; suppress the warning.
  (void) yynerrs;
};

expr: expr_function;

expr_function
  : ID ':' expr_function
    { auto me = state->exprs.addExprLambda(CUR_POS, state->symbols.create($1), nullptr, $3);
      $$ = me;
      SET_DOC_POS(me, @1);
    }
  | formal_set ':' expr_function[body]
    { auto me = state->exprs.addExprLambda(CUR_POS, state->validateFormals($formal_set), $body);
      $$ = me;
      SET_DOC_POS(me, @1);
    }
  | formal_set '@' ID ':' expr_function[body]
    {
      auto arg = state->symbols.create($ID);
      auto me = state->exprs.addExprLambda(CUR_POS, arg, state->validateFormals($formal_set, CUR_POS, arg), $body);
      $$ = me;
      SET_DOC_POS(me, @1);
    }
  | ID '@' formal_set ':' expr_function[body]
    {
      auto arg = state->symbols.create($ID);
      auto me = state->exprs.addExprLambda(CUR_POS, arg, state->validateFormals($formal_set, CUR_POS, arg), $body);
      $$ = me;
      SET_DOC_POS(me, @1);
    }
  | ASSERT expr ';' expr_function
    { $$ = state->exprs.addExprAssert(CUR_POS, $2, $4); }
  | WITH expr ';' expr_function
    { $$ = state->exprs.addExprWith(CUR_POS, $2, $4); }
  | LET binds IN_KW expr_function
    { if (!state->exprs.ERtoEP($2)->dynamicAttrs.empty())
        throw ParseError({
            .msg = HintFmt("dynamic attributes not allowed in let"),
            .pos = state->positions[CUR_POS]
        });
      $$ = state->exprs.addExprLet($2, $4);
    }
  | expr_if
  ;

expr_if
  : IF expr THEN expr ELSE expr { $$ = state->exprs.addExprIf(CUR_POS, $2, $4, $6); }
  | expr_pipe_from
  | expr_pipe_into
  | expr_op
  ;

expr_pipe_from
  : expr_op PIPE_FROM expr_pipe_from { $$ = makeCall(state->exprs, state->at(@2), $1, $3); }
  | expr_op PIPE_FROM expr_op        { $$ = makeCall(state->exprs, state->at(@2), $1, $3); }
  ;

expr_pipe_into
  : expr_pipe_into PIPE_INTO expr_op { $$ = makeCall(state->exprs, state->at(@2), $3, $1); }
  | expr_op        PIPE_INTO expr_op { $$ = makeCall(state->exprs, state->at(@2), $3, $1); }
  ;

expr_op
  : '!' expr_op %prec NOT { $$ = state->exprs.addExprOpNot($2); }
  | '-' expr_op %prec NEGATE { $$ = state->exprs.addExprCall(CUR_POS, state->exprs.addExprVar(state->s.sub), {state->exprs.addExprInt(state->values, 0), $2}); }
  | expr_op EQ expr_op { $$ = state->exprs.addExprOpEq($1, $3); }
  | expr_op NEQ expr_op { $$ = state->exprs.addExprOpNEq($1, $3); }
  | expr_op '<' expr_op { $$ = state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.lessThan), {$1, $3}); }
  | expr_op LEQ expr_op { $$ = state->exprs.addExprOpNot(state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.lessThan), {$3, $1})); }
  | expr_op '>' expr_op { $$ = state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.lessThan), {$3, $1}); }
  | expr_op GEQ expr_op { $$ = state->exprs.addExprOpNot(state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.lessThan), {$1, $3})); }
  | expr_op AND expr_op { $$ = state->exprs.addExprOpAnd(state->at(@2), $1, $3); }
  | expr_op OR expr_op { $$ = state->exprs.addExprOpOr(state->at(@2), $1, $3); }
  | expr_op IMPL expr_op { $$ = state->exprs.addExprOpImpl(state->at(@2), $1, $3); }
  | expr_op UPDATE expr_op { $$ = state->exprs.addExprOpUpdate(state->at(@2), $1, $3); }
  | expr_op '?' attrpath { $$ = state->exprs.addExprOpHasAttr($1, std::move(*$3)); delete $3; }
  | expr_op '+' expr_op
    { $$ = state->exprs.addExprConcatStrings(state->at(@2), false, new std::vector<std::pair<PosIdx, ExprRef> >({{state->at(@1), $1}, {state->at(@3), $3}})); }
  | expr_op '-' expr_op { $$ = state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.sub), {$1, $3}); }
  | expr_op '*' expr_op { $$ = state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.mul), {$1, $3}); }
  | expr_op '/' expr_op { $$ = state->exprs.addExprCall(state->at(@2), state->exprs.addExprVar(state->s.div), {$1, $3}); }
  | expr_op CONCAT expr_op { $$ = state->exprs.addExprOpConcatLists(state->at(@2), $1, $3); }
  | expr_app
  ;

expr_app
  : expr_app expr_select { $$ = makeCall(state->exprs, CUR_POS, $1, $2); state->exprs.ERtoEP($2)->warnIfCursedOr(state->symbols, state->positions); }
  | /* Once a ‘cursed or’ reaches this nonterminal, it is no longer cursed,
       because the uncursed parse would also produce an expr_app. But we need
       to remove the cursed status in order to prevent valid things like
       `f (g or)` from triggering the warning. */
    expr_select { $$ = $1; state->exprs.ERtoEP($$)->resetCursedOr(); }
  ;

expr_select
  : expr_simple '.' attrpath
    { $$ = state->exprs.addExprSelect(CUR_POS, $1, std::move(*$3), ExprRef::null); delete $3; }
  | expr_simple '.' attrpath OR_KW expr_select
    { $$ = state->exprs.addExprSelect(CUR_POS, $1, std::move(*$3), $5); delete $3; state->exprs.ERtoEP($5)->warnIfCursedOr(state->symbols, state->positions); }
  | /* Backwards compatibility: because Nixpkgs has a function named ‘or’,
       allow stuff like ‘map or [...]’. This production is problematic (see
       https://github.com/NixOS/nix/issues/11118) and will be refactored in the
       future by treating `or` as a regular identifier. The refactor will (in
       very rare cases, we think) change the meaning of expressions, so we mark
       the ExprCall with data (establishing that it is a ‘cursed or’) that can
       be used to emit a warning when an affected expression is parsed. */
    expr_simple OR_KW
    { $$ = state->exprs.addExprCall(CUR_POS, $1, {state->exprs.addExprVar(CUR_POS, state->s.or_)}, state->positions.add(state->origin, @$.endOffset)); }
  | expr_simple
  ;

expr_simple
  : ID {
      std::string_view s = "__curPos";
      if ($1.l == s.size() && strncmp($1.p, s.data(), s.size()) == 0)
          $$ = state->exprs.addExprPos(CUR_POS);
      else
          $$ = state->exprs.addExprVar(CUR_POS, state->symbols.create($1));
  }
  | INT_LIT { $$ = state->exprs.addExprInt(state->values, $1); }
  | FLOAT_LIT { $$ = state->exprs.addExprFloat(state->values, $1); }
  | '"' string_parts '"' { $$ = $2; }
  | IND_STRING_OPEN ind_string_parts IND_STRING_CLOSE {
      $$ = state->stripIndentation(CUR_POS, std::move(*$2));
      delete $2;
  }
  | path_start PATH_END
  | path_start string_parts_interpolated PATH_END {
      $2->insert($2->begin(), {state->at(@1), $1});
      $$ = state->exprs.addExprConcatStrings(CUR_POS, false, $2);
  }
  | SPATH {
      std::string path($1.p + 1, $1.l - 2);
      $$ = state->exprs.addExprCall(CUR_POS,
          state->exprs.addExprVar(state->s.findFile),
          {state->exprs.addExprVar(state->s.nixPath),
           state->exprs.addExprString(state->values, std::move(path))});
  }
  | URI {
      static bool noURLLiterals = experimentalFeatureSettings.isEnabled(Xp::NoUrlLiterals);
      if (noURLLiterals)
          throw ParseError({
              .msg = HintFmt("URL literals are disabled"),
              .pos = state->positions[CUR_POS]
          });
      $$ = state->exprs.addExprString(state->values, std::string($1));
  }
  | '(' expr ')' { $$ = $2; }
  /* Let expressions `let {..., body = ...}' are just desugared
     into `(rec {..., body = ...}).body'. */
  | LET '{' binds '}'
    { state->exprs.ERtoEP($3)->recursive = true; state->exprs.ERtoEP($3)->pos = CUR_POS; $$ = state->exprs.addExprSelect(noPos, $3, state->s.body); }
  | REC '{' binds '}'
    { state->exprs.ERtoEP($3)->recursive = true; state->exprs.ERtoEP($3)->pos = CUR_POS; $$ = $3; }
  | '{' binds1 '}'
    { state->exprs.ERtoEP($2)->pos = CUR_POS; $$ = $2; }
  | '{' '}'
    { $$ = state->exprs.addExprAttrs(CUR_POS); }
  | '[' expr_list ']' { $$ = $2; }
  ;

string_parts
  : STR { $$ = state->exprs.addExprString(state->values, std::string($1)); }
  | string_parts_interpolated { $$ = state->exprs.addExprConcatStrings(CUR_POS, true, $1); }
  | { $$ = state->exprs.addExprString(state->values, ""); }
  ;

string_parts_interpolated
  : string_parts_interpolated STR
  { $$ = $1; $1->emplace_back(state->at(@2), state->exprs.addExprString(state->values, std::string($2))); }
  | string_parts_interpolated DOLLAR_CURLY expr '}' { $$ = $1; $1->emplace_back(state->at(@2), $3); }
  | DOLLAR_CURLY expr '}' { $$ = new std::vector<std::pair<PosIdx, ExprRef>>; $$->emplace_back(state->at(@1), $2); }
  | STR DOLLAR_CURLY expr '}' {
      $$ = new std::vector<std::pair<PosIdx, ExprRef>>;
      $$->emplace_back(state->at(@1), state->exprs.addExprString(state->values, std::string($1)));
      $$->emplace_back(state->at(@2), $3);
    }
  ;

path_start
  : PATH {
    std::string_view literal({$1.p, $1.l});

    /* check for short path literals */
    if (state->settings.warnShortPathLiterals && literal.front() != '/' && literal.front() != '.') {
        logWarning({
            .msg = HintFmt("relative path literal '%s' should be prefixed with '.' for clarity: './%s'. (" ANSI_BOLD "warn-short-path-literals" ANSI_NORMAL " = true)", literal, literal),
            .pos = state->positions[CUR_POS]
        });
    }

    Path path(absPath(literal, state->basePath.path.abs()));
    /* add back in the trailing '/' to the first segment */
    if (literal.size() > 1 && literal.back() == '/')
      path += '/';
    $$ =
        /* Absolute paths are always interpreted relative to the
           root filesystem accessor, rather than the accessor of the
           current Nix expression. */
        literal.front() == '/'
        ? state->exprs.addExprPath(state->values, state->rootFS, std::move(path))
        : state->exprs.addExprPath(state->values, state->basePath.accessor, std::move(path));
  }
  | HPATH {
    if (state->settings.pureEval) {
        throw Error(
            "the path '%s' can not be resolved in pure mode",
            std::string_view($1.p, $1.l)
        );
    }
    Path path(getHome() + std::string($1.p + 1, $1.l - 1));
    $$ = state->exprs.addExprPath(state->values, ref<SourceAccessor>(state->rootFS), std::move(path));
  }
  ;

ind_string_parts
  : ind_string_parts IND_STR { $$ = $1; $1->emplace_back(state->at(@2), $2); }
  | ind_string_parts DOLLAR_CURLY expr '}' { $$ = $1; $1->emplace_back(state->at(@2), $3); }
  | { $$ = new std::vector<std::pair<PosIdx, std::variant<ExprRef, StringToken>>>; }
  ;

binds
  : binds1
  | { $$ = state->exprs.addExprAttrs(); }
  ;

binds1
  : binds1[accum] attrpath '=' expr ';'
    { $$ = $accum;
      state->addAttr($$, std::move(*$attrpath), @attrpath, $expr, @expr);
      delete $attrpath;
    }
  | binds[accum] INHERIT attrs ';'
    { $$ = $accum;
      for (auto & [i, iPos] : *$attrs) {
          if (state->exprs.ERtoEP($accum)->attrs.find(i.symbol) != state->exprs.ERtoEP($accum)->attrs.end())
              state->dupAttr(i.symbol, iPos, state->exprs.ERtoEP($accum)->attrs[i.symbol].pos);
          state->exprs.ERtoEP($accum)->attrs.emplace(
              i.symbol,
              ExprAttrs::AttrDef(state->exprs.addExprVar(iPos, i.symbol), iPos, ExprAttrs::AttrDef::Kind::Inherited));
      }
      delete $attrs;
    }
  | binds[accum] INHERIT '(' expr ')' attrs ';'
    { $$ = $accum;
      if (!state->exprs.ERtoEP($accum)->inheritFromExprs)
          state->exprs.ERtoEP($accum)->inheritFromExprs = std::make_unique<std::vector<ExprRef>>();
      state->exprs.ERtoEP($accum)->inheritFromExprs->push_back($expr);
      auto from = state->exprs.addExprInheritFrom(state->at(@expr), state->exprs.ERtoEP($accum)->inheritFromExprs->size() - 1);
      for (auto & [i, iPos] : *$attrs) {
          if (state->exprs.ERtoEP($accum)->attrs.find(i.symbol) != state->exprs.ERtoEP($accum)->attrs.end())
              state->dupAttr(i.symbol, iPos, state->exprs.ERtoEP($accum)->attrs[i.symbol].pos);
          state->exprs.ERtoEP($accum)->attrs.emplace(
              i.symbol,
              ExprAttrs::AttrDef(
                  state->exprs.addExprSelect(iPos, from, i.symbol),
                  iPos,
                  ExprAttrs::AttrDef::Kind::InheritedFrom));
      }
      delete $attrs;
    }
  | attrpath '=' expr ';'
    { $$ = state->exprs.addExprAttrs();
      state->addAttr($$, std::move(*$attrpath), @attrpath, $expr, @expr);
      delete $attrpath;
    }
  ;

attrs
  : attrs attr { $$ = $1; $1->emplace_back(AttrName(state->symbols.create($2)), state->at(@2)); }
  | attrs string_attr
    { $$ = $1;
      ExprString * str = dynamic_cast<ExprString *>(state->exprs.ERtoEP($2));
      if (str) {
          $$->emplace_back(AttrName(state->symbols.create(str->s)), state->at(@2));
          // XXX [speed]: we're leaking more memory
          // delete str;
      } else
          throw ParseError({
              .msg = HintFmt("dynamic attributes not allowed in inherit"),
              .pos = state->positions[state->at(@2)]
          });
    }
  | { $$ = new std::vector<std::pair<AttrName, PosIdx>>; }
  ;

attrpath
  : attrpath '.' attr { $$ = $1; $1->push_back(AttrName(state->symbols.create($3))); }
  | attrpath '.' string_attr
    { $$ = $1;
      ExprString * str = dynamic_cast<ExprString *>(state->exprs.ERtoEP($3));
      if (str) {
          $$->push_back(AttrName(state->symbols.create(str->s)));
          // XXX [speed]: we're leaking more memory
          // delete str;
      } else
          $$->push_back(AttrName($3));
    }
  | attr { $$ = new std::vector<AttrName>; $$->push_back(AttrName(state->symbols.create($1))); }
  | string_attr
    { $$ = new std::vector<AttrName>;
      ExprString *str = dynamic_cast<ExprString *>(state->exprs.ERtoEP($1));
      if (str) {
          $$->push_back(AttrName(state->symbols.create(str->s)));
          // XXX [speed]: we're leaking more memory
          // delete str;
      } else
          $$->push_back(AttrName($1));
    }
  ;

attr
  : ID
  | OR_KW { $$ = {"or", 2}; }
  ;

string_attr
  : '"' string_parts '"' { $$ = $2; }
  | DOLLAR_CURLY expr '}' { $$ = $2; }
  ;

expr_list
  : expr_list expr_select { $$ = $1; state->exprs.ERtoEP($1)->elems.push_back($2); /* !!! dangerous */; state->exprs.ERtoEP($2)->warnIfCursedOr(state->symbols, state->positions); }
  | { $$ = state->exprs.addExprList(); }
  ;

formal_set
  : '{' formals ',' ELLIPSIS '}' { $$ = $formals;    $$->ellipsis = true; }
  | '{' ELLIPSIS '}'             { $$ = new Formals; $$->ellipsis = true; }
  | '{' formals ',' '}'          { $$ = $formals;    $$->ellipsis = false; }
  | '{' formals '}'              { $$ = $formals;    $$->ellipsis = false; }
  | '{' '}'                      { $$ = new Formals; $$->ellipsis = false; }
  ;

formals
  : formals[accum] ',' formal
    { $$ = $accum; $$->formals.emplace_back(*$formal); delete $formal; }
  | formal
    { $$ = new Formals; $$->formals.emplace_back(*$formal); delete $formal; }
  ;

formal
  : ID { $$ = new Formal{CUR_POS, state->symbols.create($1), ExprRef::null}; }
  | ID '?' expr { $$ = new Formal{CUR_POS, state->symbols.create($1), $3}; }
  ;

%%

#include "nix/expr/eval.hh"


namespace nix {

Expr * parseExprFromBuf(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    SymbolTable & symbols,
    const EvalSettings & settings,
	Values & values,
	Exprs & exprs,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS,
    const Expr::AstSymbols & astSymbols)
{
    yyscan_t scanner;
    LexerState lexerState {
        .positionToDocComment = docComments,
        .positions = positions,
        .origin = positions.addOrigin(origin, length),
    };
    ParserState state {
        .lexerState = lexerState,
	    .values = values,
		.exprs = exprs,
        .symbols = symbols,
        .positions = positions,
        .basePath = basePath,
        .origin = lexerState.origin,
        .rootFS = rootFS,
        .s = astSymbols,
        .settings = settings,
    };

    yylex_init_extra(&lexerState, &scanner);
    Finally _destroy([&] { yylex_destroy(scanner); });

    yy_scan_buffer(text, length, scanner);
    yyparse(scanner, &state);

    return state.result;
}


}
