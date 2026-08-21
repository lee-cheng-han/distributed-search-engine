# Query planning

Planning is the validation and normalization boundary between parsed user syntax and execution. The
parser deliberately accepts a schema-independent syntax tree; `QueryPlanner` converts that tree into
an immutable `PlannedQuery` tied to the current index schema and corpus statistics.

The planner performs the following work:

- validates configured default fields, explicit fields, boosts, query shape, and range compatibility;
- expands unfielded terms and phrases into their configured default fields;
- invokes each field's schema-owned analyzer and stores the resulting terms and positions in the plan;
- folds schema field boosts and query boosts into scoring leaves;
- canonicalizes integer bounds and rejects malformed or reversed typed ranges;
- removes repeated analyzed terms and equivalent Boolean clauses;
- sorts conjunction children by estimated match count so selective posting lists execute first;
- sorts disjunctions canonically for deterministic plans and stable future cache keys.

Planning has explicit limits for AST nodes, analyzed tokens, phrase tokens, and nesting depth. Limit
violations, analyzer failures, unknown fields, invalid types, and malformed AST nodes are reported as
structured `PlanningError` values before posting-list evaluation begins.

`canonicalize` produces an unambiguous length-prefixed representation of a plan. It is suitable as
the query component of a future cache key, but a complete cache key must also include schema identity,
index generation, ranking configuration, and other result-affecting options. Cost-based conjunction
ordering can change when corpus statistics change.

The plan owns all strings, analyzed tokens, and child nodes. Execution may therefore reuse a plan
without repeating analysis; callers must still ensure that the index/schema snapshot used to execute
it is compatible with the one used to create it.
