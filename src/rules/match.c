#include "rules.h"
#include "ruledefs.h"
#include "module.h"

static SchemaRule *parsePrefixRule(ArgsCursor *ac, QueryError *err) {
  const char *prefix;
  if (AC_GetString(ac, &prefix, NULL, 0) != AC_OK) {
    QueryError_SetError(err, QUERY_EPARSEARGS, "Missing prefix");
    return NULL;
  }
  SchemaPrefixRule *ret = rm_calloc(1, sizeof(*ret));
  ret->rtype = SCRULE_TYPE_KEYPREFIX;
  ret->prefix = rm_strdup(prefix);
  ret->nprefix = strlen(ret->prefix);
  return (SchemaRule *)ret;
}

static SchemaRule *parseWildcardRule(ArgsCursor *ac, QueryError *err) {
  SchemaRule *r = rm_calloc(1, sizeof(*r));
  r->rtype = SCRULE_TYPE_MATCHALL;
  return r;
}

static SchemaRule *parseHasfieldRule(ArgsCursor *ac, QueryError *err) {
  const char *field;
  size_t nfield = 0;
  if (AC_GetString(ac, &field, &nfield, 0) != AC_OK) {
    QueryError_SetError(err, QUERY_EPARSEARGS, "Missing field");
    return NULL;
  }
  SchemaHasFieldRule *ret = rm_calloc(1, sizeof(*ret));
  ret->rtype = SCRULE_TYPE_HASFIELD;
  ret->field = RedisModule_CreateString(RSDummyContext, field, nfield);
  return (SchemaRule *)ret;
}

static SchemaRule *parseExprRule(ArgsCursor *ac, QueryError *err) {
  const char *expr;
  SchemaExprRule *erule = NULL;
  if (AC_GetString(ac, &expr, NULL, 0) != AC_OK) {
    QueryError_SetError(err, QUERY_EPARSEARGS, "Missing expression");
    goto expr_err;
  }

  erule = rm_calloc(1, sizeof(*erule));
  erule->rtype = SCRULE_TYPE_EXPRESSION;
  if (!(erule->exprobj = ExprAST_Parse(expr, strlen(expr), err))) {
    goto expr_err;
  }
  erule->exprstr = rm_strdup(expr);
  RLookup_Init(&erule->lk, NULL);
  erule->lk.options |= RLOOKUP_OPT_UNRESOLVED_OK;
  if (ExprAST_GetLookupKeys(erule->exprobj, &erule->lk, err) != EXPR_EVAL_OK) {
    goto expr_err;
  }
  for (RLookupKey *kk = erule->lk.head; kk; kk = kk->next) {
    kk->flags |= RLOOKUP_F_DOCSRC;
  }
  return (SchemaRule *)erule;

expr_err:
  if (erule) {
    if (erule->exprobj) {
      RSExpr_Free(erule->exprobj);
    }
    rm_free(erule->exprstr);
    RLookup_Cleanup(&erule->lk);
    rm_free(erule);
  }
  return NULL;
}

static int extractAction(const char *atype, SchemaAction *action, ArgsCursor *ac, QueryError *err) {
  if (!strcasecmp(atype, "INDEX")) {
    action->atype = SCACTION_TYPE_INDEX;
  } else if (!strcasecmp(atype, "ABORT")) {
    action->atype = SCACTION_TYPE_ABORT;
  } else if (!strcasecmp(atype, "GOTO")) {
    const char *target;
    if (AC_GetString(ac, &target, NULL, 0) != AC_OK) {
      QueryError_SetError(err, QUERY_EPARSEARGS, "Missing GOTO target");
      return REDISMODULE_ERR;
    }
    action->atype = SCACTION_TYPE_GOTO;
    action->u.goto_ = rm_strdup(target);
  } else if (!strcasecmp(atype, "SETATTRS")) {
    // pairwise name/value
    ArgsCursor sub_ac = {0};
    if (AC_GetVarArgs(ac, &sub_ac) != AC_OK) {
      QueryError_SetErrorFmt(err, QUERY_EPARSEARGS, "Missing attributes for action");
      return REDISMODULE_ERR;
    }

    if (AC_NumRemaining(&sub_ac) % 2 != 0) {
      QueryError_SetError(err, QUERY_EPARSEARGS, "Attributes must be specified in key/value pairs");
    }
    const char *langstr = NULL;
    double score = 0;
    ACArgSpec specs[] = {
        {.name = "LANGUAGE", .target = &langstr, .type = AC_ARGTYPE_STRING},
        {.name = "SCORE", .target = &score, .type = AC_ARGTYPE_DOUBLE, .intflags = AC_F_GE0},
        {NULL}};
    ACArgSpec *errspec = NULL;
    int rc = AC_ParseArgSpec(&sub_ac, specs, &errspec);
    if (rc != AC_OK) {
      QueryError_SetErrorFmt(err, QUERY_EPARSEARGS, "Couldn't parse SETATTR arguments: %s",
                             AC_Strerror(rc));
    }
    struct SchemaSetattrSettings *setattr = &action->u.setattr;
    if (langstr) {
      if ((setattr->attrs.language = RSLanguage_Find(langstr)) == RS_LANG_UNSUPPORTED) {
        QueryError_SetErrorFmt(err, QUERY_ENOOPTION, "Language `%s` not supported", langstr);
        return REDISMODULE_ERR;
      }
      setattr->mask |= SCATTR_TYPE_LANGUAGE;
    }
    if (score) {
      setattr->attrs.score = score;
      setattr->mask |= SCATTR_TYPE_SCORE;
    }
    action->atype = SCACTION_TYPE_SETATTR;

  } else if (!strcasecmp(atype, "LOADATTRS")) {
    struct SchemaLoadattrSettings *lattr = &action->u.loadattr;
    const char *langstr = NULL;
    const char *scorestr = NULL;
    ACArgSpec specs[] = {{.name = "LANGUAGE", .target = &langstr, .type = AC_ARGTYPE_STRING},
                         {.name = "SCORE", .target = &scorestr, .type = AC_ARGTYPE_STRING},
                         {NULL}};
    ArgsCursor sub_ac = {0};
    if (AC_GetVarArgs(ac, &sub_ac) != AC_OK) {
      QueryError_SetErrorFmt(err, QUERY_EPARSEARGS, "Missing attributes for action");
      return REDISMODULE_ERR;
    }
    ACArgSpec *errspec = NULL;
    int rc = AC_ParseArgSpec(&sub_ac, specs, &errspec);
    if (rc != AC_OK) {
      QueryError_SetErrorFmt(err, QUERY_EPARSEARGS, "Couldn't parse SETATTR arguments: %s",
                             AC_Strerror(rc));
    }
    if (langstr) {
      lattr->langfield = RedisModule_CreateString(RSDummyContext, langstr, strlen(langstr));
    }
    if (scorestr) {
      lattr->scorefield = RedisModule_CreateString(RSDummyContext, scorestr, strlen(scorestr));
    }
    action->atype = SCACTION_TYPE_LOADATTR;
  } else {
    QueryError_SetErrorFmt(err, QUERY_EPARSEARGS, "Unknown action type `%s`", atype);
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

static int matchExpression(const SchemaRule *r, RedisModuleCtx *ctx, RuleKeyItem *item) {
  SchemaExprRule *e = (SchemaExprRule *)r;
  int rc = 0;
  RLookupRow row = {0};
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, NULL);
  QueryError status = {0};
  RLookupLoadOptions loadopts = {
      .sctx = &sctx, .status = &status, .mode = RLOOKUP_LOAD_LKKEYS, .noSortables = 1};
  if (item->kobj) {
    loadopts.ktype = RLOOKUP_KEY_OBJ;
    loadopts.key.kobj = item->kobj;
  } else {
    loadopts.ktype = RLOOKUP_KEY_RSTR;
    loadopts.key.rstr = item->kstr;
  }
  RSValue rsv = RSVALUE_STATIC;

  if (RLookup_LoadDocument(&e->lk, &row, &loadopts) != REDISMODULE_OK) {
    // printf("Couldn't load document: %s\n", QueryError_GetError(&status));
    goto done;
  }

  ExprEval eval = {.err = &status, .lookup = &e->lk, .srcrow = &row, .root = e->exprobj};
  if (ExprEval_Eval(&eval, &rsv) != EXPR_EVAL_OK) {
    // printf("Couldn't evaluate expression: %s\n", QueryError_GetError(&status));
    goto done;
  }

  rc = RSValue_BoolTest(&rsv);

done:
  RLookupRow_Cleanup(&row);
  QueryError_ClearError(&status);
  RSValue_Clear(&rsv);
  return rc;
}

static int matchPrefix(const SchemaRule *r, RedisModuleCtx *ctx, RuleKeyItem *item) {
  SchemaPrefixRule *prule = (SchemaPrefixRule *)r;
  size_t n;
  const char *s = RedisModule_StringPtrLen(item->kstr, &n);
  if (prule->nprefix > n) {
    return 0;
  }
  int ret = strncmp(prule->prefix, s, prule->nprefix) == 0;
  return ret;
}

static int matchAll(const SchemaRule *r, RedisModuleCtx *ctx, RuleKeyItem *item) {
  return 1;
}

static int matchHasfield(const SchemaRule *r, RedisModuleCtx *ctx, RuleKeyItem *item) {
  SchemaHasFieldRule *hrule = (SchemaHasFieldRule *)r;
  size_t n;
  if (!item->kobj) {
    item->kobj = RedisModule_OpenKey(ctx, item->kstr, REDISMODULE_READ);
    if (!item->kobj) {
      return 0;
    }
  }
  int ret = 0;
  RedisModule_HashGet(item->kobj, REDISMODULE_HASH_EXISTS, hrule->field, &ret, NULL);
  return ret;
}

void SchemaRule_Free(SchemaRule *r) {
  switch (r->rtype) {
    case SCRULE_TYPE_EXPRESSION: {
      SchemaExprRule *serule = (SchemaExprRule *)r;
      RSExpr_Free(serule->exprobj);
      rm_free(serule->exprstr);
      break;
    }
    case SCRULE_TYPE_HASFIELD: {
      SchemaHasFieldRule *hfrule = (SchemaHasFieldRule *)r;
      rm_free(hfrule->field);
      break;
    }
    case SCRULE_TYPE_KEYPREFIX: {
      SchemaPrefixRule *prule = (SchemaPrefixRule *)r;
      rm_free(prule->prefix);
      break;
    }
    case SCRULE_TYPE_MATCHALL:
      break;
  }
  SchemaAction *action = &r->action;
  switch (action->atype) {
    case SCACTION_TYPE_GOTO:
      rm_free(action->u.goto_);
      break;
    case SCACTION_TYPE_LOADATTR:
      if (action->u.loadattr.scorefield) {
        RedisModule_FreeString(RSDummyContext, action->u.loadattr.scorefield);
      }
      if (action->u.loadattr.langfield) {
        RedisModule_FreeString(RSDummyContext, action->u.loadattr.langfield);
      }
    case SCACTION_TYPE_SETATTR:
    case SCACTION_TYPE_ABORT:
    case SCACTION_TYPE_INDEX:
    default:
      break;
  }

  size_t n = array_len(r->rawrule);
  for (size_t ii = 0; ii < n; ++ii) {
    rm_free(r->rawrule[ii]);
  }
  array_free(r->rawrule);

  rm_free(r->name);
  rm_free(r->index);
  rm_free(r);
}

int SchemaRules_AddArgsInternal(SchemaRules *rules, const char *index, const char *name,
                                ArgsCursor *ac, QueryError *err) {
  // First argument is the name...
  size_t beginpos = AC_Tell(ac);
  const char *rtype = NULL;
  int rc = AC_GetString(ac, &rtype, NULL, 0);
  if (rc != AC_OK) {
    QueryError_SetError(err, QUERY_EPARSEARGS, "Missing type for rule");
    return REDISMODULE_ERR;
  }

  SchemaRule *r = NULL;
  if (!strcasecmp(rtype, "PREFIX")) {
    r = parsePrefixRule(ac, err);
  } else if (!strcasecmp(rtype, "EXPR")) {
    r = parseExprRule(ac, err);
  } else if (!strcasecmp(rtype, "HASFIELD")) {
    r = parseHasfieldRule(ac, err);
  } else if (!strcasecmp(rtype, "*")) {
    r = parseWildcardRule(ac, err);
  } else {
    QueryError_SetErrorFmt(err, QUERY_ENOOPTION, "No such match type `%s`\n", rtype);
    return REDISMODULE_ERR;
  }

  if (!r) {
    return REDISMODULE_ERR;
  }
  const char *astr = NULL;
  if (AC_GetString(ac, &astr, NULL, 0) != AC_OK) {
    astr = "INDEX";
  }
  if (extractAction(astr, &r->action, ac, err) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }
  r->index = rm_strdup(index);
  r->name = rm_strdup(name);
  r->rawrule = array_new(char *, ac->argc);

  AC_Seek(ac, beginpos);
  while (AC_NumRemaining(ac)) {
    char *s = rm_strdup(AC_GetStringNC(ac, NULL));
    r->rawrule = array_append(r->rawrule, s);
  }
  AC_Seek(ac, beginpos);
  *array_ensure_tail(&rules->rules, SchemaRule *) = r;
  return REDISMODULE_OK;
}

/**
 * The idea here is to allow multiple rule matching types, and to have a dynamic
 * function table for each rule type
 */
typedef int (*scruleMatchFn)(const SchemaRule *, RedisModuleCtx *, RuleKeyItem *);

static scruleMatchFn matchfuncs_g[] = {[SCRULE_TYPE_KEYPREFIX] = matchPrefix,
                                       [SCRULE_TYPE_EXPRESSION] = matchExpression,
                                       [SCRULE_TYPE_HASFIELD] = matchHasfield,
                                       [SCRULE_TYPE_MATCHALL] = matchAll};

static void loadAttrFields(RuleKeyItem *item, struct SchemaLoadattrSettings *lattr,
                           MatchAction *curAction) {
  // Load the key
  if (!item->kobj) {
    item->kobj = RedisModule_OpenKey(RSDummyContext, item->kstr, REDISMODULE_READ);
  }
  if (!item->kobj) {
    return;
  }

  if (lattr->langfield) {
    RedisModuleString *langstr = NULL;
    RedisModule_HashGet(item->kobj, 0, lattr->langfield, &langstr, NULL);
    if (langstr) {
      RSLanguage lang = RSLanguage_Find(RedisModule_StringPtrLen(langstr, NULL));
      if (lang != RS_LANG_UNSUPPORTED) {
        curAction->attrs.language = lang;
      } else {
        // Send warning about invalid language?
      }
    }
  }

  if (lattr->scorefield) {
    RedisModuleString *scorestr = NULL;
    RedisModule_HashGet(item->kobj, 0, lattr->scorefield, &scorestr, NULL);
    double d = 0;
    if (scorestr) {
      int rc = RedisModule_StringToDouble(scorestr, &d);
      if (rc == REDISMODULE_OK) {
        curAction->attrs.score = d;
      } else {
        // send warning about bad score
      }
    }
  }
}

int SchemaRules_Check(const SchemaRules *rules, RedisModuleCtx *ctx, RuleKeyItem *item,
                      MatchAction **results, size_t *nresults) {
  array_clear(rules->actions);
  *results = rules->actions;
  size_t nrules = array_len(rules->rules);
  for (size_t ii = 0; ii < nrules; ++ii) {
  eval_rule:;
    SchemaRule *rule = rules->rules[ii];
    scruleMatchFn fn = matchfuncs_g[rule->rtype];
    if (!fn(rule, ctx, item)) {
      continue;
    }

    // MatchActions contain the settings, per-index; we first find the result for
    // the given index, and if we can't find it, we create it.
    switch (rule->action.atype) {
      case SCACTION_TYPE_ABORT:
        goto end_match;
      case SCACTION_TYPE_INDEX:
      case SCACTION_TYPE_SETATTR:
      case SCACTION_TYPE_LOADATTR:
        break;
      case SCACTION_TYPE_GOTO: {
        for (size_t jj = ii; jj < nrules; ++jj) {
          if (!strcmp(rule->action.u.goto_, rules->rules[jj]->name)) {
            ii = jj;
            goto eval_rule;
          }
        }
        goto next_rule;
      }
    }

    MatchAction *curAction = NULL;
    for (size_t ii = 0; ii < *nresults; ++ii) {
      if (!strcmp((*results)[ii].index, rule->index)) {
        curAction = (*results) + ii;
      }
    }
    if (!curAction) {
      curAction = array_ensure_tail(results, MatchAction);
      curAction->index = rule->index;
      curAction->attrs.language = 0;
      curAction->attrs.score = 0;
    }
    if (rule->action.atype == SCACTION_TYPE_SETATTR) {
      const IndexItemAttrs *attr = &rule->action.u.setattr.attrs;
      int mask = rule->action.u.setattr.mask;
      if (mask & SCATTR_TYPE_LANGUAGE) {
        curAction->attrs.language = attr->language;
      }
      if (mask & SCATTR_TYPE_SCORE) {
        curAction->attrs.score = attr->score;
      }
    } else if (rule->action.atype == SCACTION_TYPE_LOADATTR) {
      loadAttrFields(item, &rule->action.u.loadattr, curAction);
    }
  next_rule:;
  }
end_match:
  *nresults = array_len(*results);
  return *nresults;
}