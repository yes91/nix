#include "nixexpr.hh"
#include "parser.hh"
#include "hash.hh"
#include "util.hh"
#include "nixexpr-ast.hh"

#include <cstdlib>

using namespace nix;


struct Env;
struct Value;

typedef std::map<string, Value> Bindings;


struct Env
{
    Env * up;
    Bindings bindings;
};


typedef enum {
    tInt = 1,
    tAttrs,
    tThunk,
    tLambda,
    tCopy
} ValueType;


struct Value
{
    ValueType type;
    union 
    {
        int integer;
        Bindings * attrs;
        struct {
            Env * env;
            Expr expr;
        } thunk;
        struct {
            Env * env;
            Pattern pat;
            Expr body;
        } lambda;
        Value * val;
    };
};


std::ostream & operator << (std::ostream & str, Value & v)
{
    switch (v.type) {
    case tInt:
        str << v.integer;
        break;
    case tAttrs:
        str << "{ ";
        foreach (Bindings::iterator, i, *v.attrs)
            str << i->first << " = " << i->second << "; ";
        str << "}";
        break;
    case tThunk:
        str << "<CODE>";
        break;
    case tLambda:
        str << "<LAMBDA>";
        break;
    default:
        abort();
    }
    return str;
}


static void eval(Env * env, Expr e, Value & v);


void forceValue(Value & v)
{
    if (v.type == tThunk) eval(v.thunk.env, v.thunk.expr, v);
    else if (v.type == tCopy) {
        forceValue(*v.val);
        v = *v.val;
    }
}


Value * lookupVar(Env * env, const string & name)
{
    for ( ; env; env = env->up) {
        Bindings::iterator i = env->bindings.find(name);
        if (i != env->bindings.end()) return &i->second;
    }
    throw Error("undefined variable");
}


unsigned long nrValues = 0;

unsigned long nrEnvs = 0;

Env * allocEnv()
{
    nrEnvs++;
    return new Env;
}


static bool patternIsStrict(Pattern pat)
{
    ATerm name, ellipsis, pat1, pat2;
    ATermList formals;
    if (matchVarPat(pat, name)) return false;
    else if (matchAttrsPat(pat, formals, ellipsis)) return true;
    else if (matchAtPat(pat, pat1, pat2))
        return patternIsStrict(pat1) || patternIsStrict(pat2);
    else abort();
}


static void bindVarPats(Pattern pat, Env & newEnv,
    Env * argEnv, Expr argExpr, Value * & vArg)
{
    Pattern pat1, pat2;
    if (matchAtPat(pat, pat1, pat2)) {
        bindVarPats(pat1, newEnv, argEnv, argExpr, vArg);
        bindVarPats(pat2, newEnv, argEnv, argExpr, vArg);
        return;
    }

    ATerm name;
    if (!matchVarPat(pat, name)) abort();

    if (vArg) {
        Value & v = newEnv.bindings[aterm2String(name)];
        v.type = tCopy;
        v.val = vArg;
    } else {
        vArg = &newEnv.bindings[aterm2String(name)];
        vArg->type = tThunk;
        vArg->thunk.env = argEnv;
        vArg->thunk.expr = argExpr;
    }
}


static void bindAttrPats(Pattern pat, Env & newEnv,
    Value & vArg, Value * & vArgInEnv)
{
    Pattern pat1, pat2;
    if (matchAtPat(pat, pat1, pat2)) {
        bindAttrPats(pat1, newEnv, vArg, vArgInEnv);
        bindAttrPats(pat2, newEnv, vArg, vArgInEnv);
        return;
    }
    
    ATerm name;
    if (matchVarPat(pat, name)) {
        if (vArgInEnv) {
            Value & v = newEnv.bindings[aterm2String(name)];
            v.type = tCopy;
            v.val = vArgInEnv;
        } else {
            vArgInEnv = &newEnv.bindings[aterm2String(name)];
            *vArgInEnv = vArg;
        }
        return;
    }

    ATerm ellipsis;
    ATermList formals;
    if (matchAttrsPat(pat, formals, ellipsis)) {
        for (ATermIterator i(formals); i; ++i) {
            Expr name, def;
            DefaultValue def2;
            if (!matchFormal(*i, name, def2)) abort(); /* can't happen */

            Bindings::iterator j = vArg.attrs->find(aterm2String(name));
            if (j == vArg.attrs->end())
                throw TypeError(format("the argument named `%1%' required by the function is missing")
                    % aterm2String(name));
            
            Value & v = newEnv.bindings[aterm2String(name)];
            v.type = tCopy;
            v.val = &j->second;
        }
        return;
    }

    abort();
}


static void eval(Env * env, Expr e, Value & v)
{
    printMsg(lvlError, format("eval: %1%") % e);

    ATerm name;
    if (matchVar(e, name)) {
        Value * v2 = lookupVar(env, aterm2String(name));
        forceValue(*v2);
        v = *v2;
        return;
    }

    int n;
    if (matchInt(e, n)) {
        v.type = tInt;
        v.integer = n;
        return;
    }

    ATermList es;
    if (matchAttrs(e, es)) {
        v.type = tAttrs;
        v.attrs = new Bindings;
        ATerm e2, pos;
        for (ATermIterator i(es); i; ++i) {
            if (!matchBind(*i, name, e2, pos)) abort(); /* can't happen */
            Value & v2 = (*v.attrs)[aterm2String(name)];
            nrValues++;
            v2.type = tThunk;
            v2.thunk.env = env;
            v2.thunk.expr = e2;
        }
        return;
    }

    ATermList rbnds, nrbnds;
    if (matchRec(e, rbnds, nrbnds)) {
        Env * env2 = allocEnv();
        env2->up = env;
        
        v.type = tAttrs;
        v.attrs = &env2->bindings;
        ATerm name, e2, pos;
        for (ATermIterator i(rbnds); i; ++i) {
            if (!matchBind(*i, name, e2, pos)) abort(); /* can't happen */
            Value & v2 = env2->bindings[aterm2String(name)];
            nrValues++;
            v2.type = tThunk;
            v2.thunk.env = env2;
            v2.thunk.expr = e2;
        }
        
        return;
    }

    Expr e2;
    if (matchSelect(e, e2, name)) {
        eval(env, e2, v);
        if (v.type != tAttrs) throw TypeError("expected attribute set");
        Bindings::iterator i = v.attrs->find(aterm2String(name));
        if (i == v.attrs->end()) throw TypeError("attribute not found");
        forceValue(i->second);
        v = i->second;
        return;
    }

    Pattern pat; Expr body; Pos pos;
    if (matchFunction(e, pat, body, pos)) {
        v.type = tLambda;
        v.lambda.env = env;
        v.lambda.pat = pat;
        v.lambda.body = body;
        return;
    }

    Expr fun, arg;
    if (matchCall(e, fun, arg)) {
        eval(env, fun, v);
        if (v.type != tLambda) throw TypeError("expected function");

        Env * env2 = allocEnv();
        env2->up = env;

        if (patternIsStrict(v.lambda.pat)) {
            Value vArg;
            eval(env, arg, vArg);
            if (vArg.type != tAttrs) throw TypeError("expected attribute set");
            Value * vArg2 = 0;
            bindAttrPats(v.lambda.pat, *env2, vArg, vArg2);
        } else {
            Value * vArg = 0;
            bindVarPats(v.lambda.pat, *env2, env, arg, vArg);
        }
        
        eval(env2, v.lambda.body, v);
        return;
    }

    abort();
}


void doTest(string s)
{
    EvalState state;
    Expr e = parseExprFromString(state, s, "/");
    printMsg(lvlError, format(">>>>> %1%") % e);
    Value v;
    eval(0, e, v);
    printMsg(lvlError, format("result: %1%") % v);
}


void run(Strings args)
{
    printMsg(lvlError, format("size of value: %1% bytes") % sizeof(Value));
    
    doTest("123");
    doTest("{ x = 1; y = 2; }");
    doTest("{ x = 1; y = 2; }.y");
    doTest("rec { x = 1; y = x; }.y");
    doTest("(x: x) 1");
    doTest("(x: y: y) 1 2");
    doTest("(x@y: x) 1");
    doTest("(x@y: y) 2");
    doTest("(x@y@z: y) 3");
    doTest("x: x");
    doTest("({x, y}: x) { x = 1; y = 2; }");
    doTest("({x, y}@args: args.x) { x = 1; y = 2; }");
    doTest("({x, y}@args@args2: args2.x) { x = 1; y = 2; }");
    
    //Expr e = parseExprFromString(state, "let x = \"a\"; in x + \"b\"", "/");
    //Expr e = parseExprFromString(state, "(x: x + \"b\") \"a\"", "/");
    //Expr e = parseExprFromString(state, "\"a\" + \"b\"", "/");
    //Expr e = parseExprFromString(state, "\"a\" + \"b\"", "/");

    printMsg(lvlError, format("alloced %1% values") % nrValues);
    printMsg(lvlError, format("alloced %1% environments") % nrEnvs);
}


void printHelp()
{
}


string programId = "eval-test";
