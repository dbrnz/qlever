// Copyright 2015, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author:
//   2015-2017 Björn Buchhold (buchhold@informatik.uni-freiburg.de)
//   2018-     Johannes Kalmbach (kalmbach@informatik.uni-freiburg.de)

#pragma once
#include <set>
#include <vector>

#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "engine/CheckUsePatternTrick.h"
#include "engine/Filter.h"
#include "engine/QueryExecutionTree.h"
#include "parser/ParsedQuery.h"

using std::vector;

class QueryPlanner {
 public:
  explicit QueryPlanner(QueryExecutionContext* qec);

  // Create the best execution tree for the given query according to the
  // optimization algorithm and cost estimates of the QueryPlanner.
  QueryExecutionTree createExecutionTree(ParsedQuery& pq);

  class TripleGraph {
   public:
    TripleGraph();

    TripleGraph(const TripleGraph& other);

    TripleGraph& operator=(const TripleGraph& other);

    TripleGraph(const TripleGraph& other, vector<size_t> keepNodes);

    struct Node {
      Node(size_t id, SparqlTriple t) : _id(id), _triple(std::move(t)) {
        if (isVariable(_triple._s)) {
          _variables.insert(_triple._s.getVariable());
        }
        if (isVariable(_triple._p)) {
          _variables.insert(Variable{_triple._p._iri});
        }
        if (isVariable(_triple._o)) {
          _variables.insert(_triple._o.getVariable());
        }
      }

      Node(size_t id, const Variable& cvar, std::vector<std::string> words,
           const vector<SparqlTriple>& trips)
          : _id(id),
            // TODO<joka921> What is this triple used for? If it is just a
            // dummy, then we can replace it by a `variant<Triple,
            // TextNodeData>`.
            _triple(cvar,
                    PropertyPath(PropertyPath::Operation::IRI, 0,
                                 INTERNAL_TEXT_MATCH_PREDICATE, {}),
                    TripleComponent::UNDEF{}),
            _cvar(cvar),
            _wordPart(std::move(words)) {
        _variables.insert(cvar);
        for (const auto& t : trips) {
          if (isVariable(t._s)) {
            _variables.insert(t._s.getVariable());
          }
          if (isVariable(t._p)) {
            _variables.insert(Variable{t._p._iri});
          }
          if (isVariable(t._o)) {
            _variables.insert(t._o.getVariable());
          }
        }
      }

      Node(const Node& other) = default;

      Node& operator=(const Node& other) = default;

      // Returns true if the two nodes equal apart from the id
      // and the order of variables
      bool isSimilar(const Node& other) const {
        return _triple == other._triple && _cvar == other._cvar &&
               _wordPart == other._wordPart && _variables == other._variables;
      }

      friend std::ostream& operator<<(std::ostream& out, const Node& n) {
        out << "id: " << n._id << " triple: " << n._triple.asString()
            << " vars_ ";
        for (const auto& s : n._variables) {
          out << s.name() << ", ";
        }
        // TODO<joka921> Should the `cvar` and the `wordPart` be stored
        // together?
        if (n._cvar.has_value()) {
          out << " cvar " << n._cvar.value().name() << " wordPart "
              << absl::StrJoin(n._wordPart.value(), " ");
        }
        return out;
      }

      size_t _id;
      SparqlTriple _triple;
      ad_utility::HashSet<Variable> _variables;
      std::optional<Variable> _cvar = std::nullopt;
      std::optional<std::vector<std::string>> _wordPart = std::nullopt;
    };

    // Allows for manually building triple graphs for testing
    explicit TripleGraph(
        const std::vector<std::pair<Node, std::vector<size_t>>>& init);

    // Checks for id and order independent equality
    bool isSimilar(const TripleGraph& other) const;
    string asString() const;

    bool isTextNode(size_t i) const;

    vector<vector<size_t>> _adjLists;
    ad_utility::HashMap<size_t, Node*> _nodeMap;
    std::list<TripleGraph::Node> _nodeStorage;

    ad_utility::HashMap<Variable, vector<size_t>> identifyTextCliques() const;

    vector<size_t> bfsLeaveOut(size_t startNode,
                               ad_utility::HashSet<size_t> leaveOut) const;

    void collapseTextCliques();

   private:
    vector<std::pair<TripleGraph, vector<SparqlFilter>>> splitAtContextVars(
        const vector<SparqlFilter>& origFilters,
        ad_utility::HashMap<string, vector<size_t>>& contextVarTotextNodes)
        const;

    vector<SparqlFilter> pickFilters(const vector<SparqlFilter>& origFilters,
                                     const vector<size_t>& nodes) const;
  };

  class SubtreePlan {
   public:
    enum Type { BASIC, OPTIONAL, MINUS };

    explicit SubtreePlan(QueryExecutionContext* qec)
        : _qet(std::make_shared<QueryExecutionTree>(qec)) {}

    template <typename Operation>
    SubtreePlan(QueryExecutionContext* qec,
                std::shared_ptr<Operation> operation)
        : _qet{std::make_shared<QueryExecutionTree>(qec,
                                                    std::move(operation))} {}

    std::shared_ptr<QueryExecutionTree> _qet;
    std::shared_ptr<ResultTable> _cachedResult;
    bool _isCached = false;
    uint64_t _idsOfIncludedNodes = 0;
    uint64_t _idsOfIncludedFilters = 0;
    Type type = Type::BASIC;

    size_t getCostEstimate() const;

    size_t getSizeEstimate() const;

    void addAllNodes(uint64_t otherNodes);
  };

  [[nodiscard]] TripleGraph createTripleGraph(
      const parsedQuery::BasicGraphPattern* pattern) const;

  void setEnablePatternTrick(bool enablePatternTrick);

  // Create a set of possible execution trees for the given parsed query. The
  // best (cheapest) execution tree according to the QueryPlanner is part of
  // that set. When the query has no `ORDER BY` clause, the set contains one
  // optimal execution tree for each possible ordering (by one column) of the
  // result. This is relevant for subqueries, which are currently optimized
  // independently from the rest of the query, but where it depends on the rest
  // of the query, which ordering of the result is best.
  [[nodiscard]] std::vector<SubtreePlan> createExecutionTrees(ParsedQuery& pq);

 private:
  QueryExecutionContext* _qec;

  // Used to count the number of unique variables created using
  // generateUniqueVarName
  size_t _internalVarCount;

  bool _enablePatternTrick;

  [[nodiscard]] std::vector<QueryPlanner::SubtreePlan> optimize(
      ParsedQuery::GraphPattern* rootPattern);

  /**
   * @brief Fills children with all operations that are associated with a single
   * node in the triple graph (e.g. IndexScans).
   */
  [[nodiscard]] vector<SubtreePlan> seedWithScansAndText(
      const TripleGraph& tg,
      const vector<vector<QueryPlanner::SubtreePlan>>& children);

  /**
   * @brief Returns a subtree plan that will compute the values for the
   * variables in this single triple. Depending on the triple's PropertyPath
   * this subtree can be arbitrarily large.
   */
  [[nodiscard]] vector<SubtreePlan> seedFromPropertyPathTriple(
      const SparqlTriple& triple);

  /**
   * @brief Returns a parsed query for the property path.
   */
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromPropertyPath(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);

  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromSequence(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromAlternative(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromTransitive(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern>
  seedFromTransitiveMin(const TripleComponent& left, const PropertyPath& path,
                        const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern>
  seedFromTransitiveMax(const TripleComponent& left, const PropertyPath& path,
                        const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromInverse(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);
  [[nodiscard]] std::shared_ptr<ParsedQuery::GraphPattern> seedFromIri(
      const TripleComponent& left, const PropertyPath& path,
      const TripleComponent& right);

  [[nodiscard]] Variable generateUniqueVarName();

  // Creates a tree of unions with the given patterns as the trees leaves
  [[nodiscard]] ParsedQuery::GraphPattern uniteGraphPatterns(
      std::vector<ParsedQuery::GraphPattern>&& patterns) const;

  /**
   * @brief Merges two rows of the dp optimization table using various types of
   * joins.
   * @return A new row for the dp table that contains plans created by joining
   * the result of a plan in a and a plan in b.
   */
  [[nodiscard]] vector<SubtreePlan> merge(const vector<SubtreePlan>& a,
                                          const vector<SubtreePlan>& b,
                                          const TripleGraph& tg) const;

  [[nodiscard]] std::vector<QueryPlanner::SubtreePlan> createJoinCandidates(
      const SubtreePlan& a, const SubtreePlan& b,
      std::optional<TripleGraph> tg) const;

  // Used internally by `createJoinCandidates`. If `a` or `b` is a transitive
  // path operation and the other input can be bound to this transitive path
  // (see `TransitivePath.cpp` for details), then returns that bound transitive
  // path. Else returns `std::nullopt`
  [[nodiscard]] static std::optional<SubtreePlan> createJoinWithTransitivePath(
      SubtreePlan a, SubtreePlan b,
      const std::vector<std::array<ColumnIndex, 2>>& jcs);

  // Used internally by `createJoinCandidates`. If  `a` or `b` is a
  // `HasPredicateScan` with a variable as a subject (`?x ql:has-predicate
  // <VariableOrIri>`) and `a` and `b` can be joined on that subject variable,
  // then returns a `HasPredicateScan` that takes the other input as a subtree.
  // Else returns `std::nullopt`.
  [[nodiscard]] static std::optional<SubtreePlan>
  createJoinWithHasPredicateScan(
      SubtreePlan a, SubtreePlan b,
      const std::vector<std::array<ColumnIndex, 2>>& jcs);

  // Used internally by `createJoinCandidates`. If  `a` or `b` is a
  // `TextOperationWithoutFilter` create a `TextOperationWithFilter` that takes
  // the result of the other input as the filter input. Else return
  // `std::nullopt`.
  [[nodiscard]] static std::optional<SubtreePlan> createJoinAsTextFilter(
      SubtreePlan a, SubtreePlan b,
      const std::vector<std::array<ColumnIndex, 2>>& jcs);

  [[nodiscard]] vector<SubtreePlan> getOrderByRow(
      const ParsedQuery& pq,
      const std::vector<std::vector<SubtreePlan>>& dpTab) const;

  [[nodiscard]] vector<SubtreePlan> getGroupByRow(
      const ParsedQuery& pq,
      const std::vector<std::vector<SubtreePlan>>& dpTab) const;

  [[nodiscard]] vector<SubtreePlan> getDistinctRow(
      const parsedQuery::SelectClause& selectClause,
      const vector<vector<SubtreePlan>>& dpTab) const;

  [[nodiscard]] vector<SubtreePlan> getPatternTrickRow(
      const parsedQuery::SelectClause& selectClause,
      const vector<vector<SubtreePlan>>& dpTab,
      const checkUsePatternTrick::PatternTrickTuple& patternTrickTuple);

  [[nodiscard]] vector<SubtreePlan> getHavingRow(
      const ParsedQuery& pq, const vector<vector<SubtreePlan>>& dpTab) const;

  [[nodiscard]] bool connected(const SubtreePlan& a, const SubtreePlan& b,
                               const TripleGraph& graph) const;

  [[nodiscard]] std::vector<std::array<ColumnIndex, 2>> getJoinColumns(
      const SubtreePlan& a, const SubtreePlan& b) const;

  [[nodiscard]] string getPruningKey(
      const SubtreePlan& plan,
      const vector<ColumnIndex>& orderedOnColumns) const;

  [[nodiscard]] void applyFiltersIfPossible(
      std::vector<SubtreePlan>& row, const std::vector<SparqlFilter>& filters,
      bool replaceInsteadOfAddPlans) const;

  /**
   * @brief Optimize a set of triples, filters and precomputed candidates
   * for child graph patterns
   *
   *
   * Optimize every GraphPattern starting with the leaves of the
   * GraphPattern tree.

   * Strategy:
   * Create a graph.
   * Each triple corresponds to a node, there is an edge between two nodes
   * iff they share a variable.

   * TripleGraph tg = createTripleGraph(&arg);

   * Each node/triple corresponds to a scan (more than one way possible),
   * each edge corresponds to a possible join.

   * Enumerate and judge possible query plans using a DP table.
   * Each ExecutionTree for a sub-problem gives an estimate:
   * There are estimates for cost and size ( and multiplicity per column).
   * Start bottom up, i.e. with the scans for triples.
   * Always merge two solutions from the table by picking one possible
   * join. A join is possible, if there is an edge between the results.
   * Therefore we keep track of all edges that touch a sub-result.
   * When joining two sub-results, the results edges are those that belong
   * to exactly one of the two input sub-trees.
   * If two of them have the same target, only one out edge is created.
   * All edges that are shared by both subtrees, are checked if they are
   * covered by the join or if an extra filter/select is needed.

   * The algorithm then creates all possible plans for 1 to n triples.
   * To generate a plan for k triples, all subsets between i and k-i are
   * joined.

   * Filters are now added to the mix when building execution plans.
   * Without them, a plan has an execution tree and a set of
   * covered triple nodes.
   * With them, it also has a set of covered filters.
   * A filter can be applied as soon as all variables that occur in the
   * filter Are covered by the query. This is also always the place where
   * this is done.

   * Text operations form cliques (all triples connected via the context
   * cvar). Detect them and turn them into nodes with stored word part and
   * edges to connected variables.

   * Each text operation has two ways how it can be used.
   * 1) As leave in the bottom row of the tab.
   * According to the number of connected variables, the operation creates
   * a cross product with n entities that can be used in subsequent joins.
   * 2) as intermediate unary (downwards) nodes in the execution tree.
   * This is a bit similar to sorts: they can be applied after each step
   * and will filter on one variable.
   * Cycles have to be avoided (by previously removing a triple and using
   * it as a filter later on).
   */
  [[nodiscard]] vector<vector<SubtreePlan>> fillDpTab(
      const TripleGraph& graph, const vector<SparqlFilter>& fs,
      const vector<vector<SubtreePlan>>& children);

  [[nodiscard]] SubtreePlan getTextLeafPlan(
      const TripleGraph::Node& node) const;

  /**
   * @brief return the index of the cheapest execution tree in the argument.
   *
   * If we are in the unit test mode, this is deterministic by additionally
   * sorting by the cache key when comparing equally cheap indices, else the
   * first element that has the minimum index is returned.
   */
  [[nodiscard]] size_t findCheapestExecutionTree(
      const std::vector<SubtreePlan>& lastRow) const;

  /// if this Planner is not associated with a queryExecutionContext we are only
  /// in the unit test mode
  [[nodiscard]] bool isInTestMode() const { return _qec == nullptr; }
};
