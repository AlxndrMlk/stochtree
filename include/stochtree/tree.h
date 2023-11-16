/*!
 * Based largely on the tree classes in xgboost and treelite, both released under the Apache license with the following copyright:
 * Copyright 2015-2023 by XGBoost Contributors
 * Copyright 2017-2021 by [treelite] Contributors
 */
#ifndef STOCHTREE_TREE_H_
#define STOCHTREE_TREE_H_

#include <stochtree/log.h>
#include <stochtree/meta.h>

#include <cstdint>
#include <set>
#include <stack>
#include <string>

namespace StochTree {

/*! \brief Tree node type */
enum class TreeNodeType : std::int8_t {
  kLeafNode = 0,
  kNumericalSplitNode = 1,
  kCategoricalSplitNode = 2
};

/*! \brief Get string representation of TreeNodeType */
std::string TreeNodeTypeToString(TreeNodeType type);

/*! \brief Get NodeType from string */
TreeNodeType TreeNodeTypeFromString(std::string const& name);

enum FeatureSplitType {
  kNumericSplit,
  kOrderedCategoricalSplit,
  kUnorderedCategoricalSplit
};

/*! \brief in-memory representation of a decision tree */
class Tree {
 public:
  static constexpr std::int32_t kInvalidNodeId{-1};
  static constexpr std::int32_t kDeletedNodeMarker = std::numeric_limits<node_t>::max();
  static constexpr std::int32_t kRoot{0};
  
  Tree() = default;
  // ~Tree() = default;
  Tree(Tree const&) = delete;
  Tree& operator=(Tree const&) = delete;
  Tree(Tree&&) noexcept = default;
  Tree& operator=(Tree&&) noexcept = default;

  Tree* Clone();

  void CloneFromTree(Tree* tree);

  /*! \brief Number of nodes */
  std::int32_t num_nodes{0};
  /*! \brief Number of deleted nodes */
  std::int32_t num_deleted_nodes{0};

  /*! \brief Reset tree to empty vectors and default values of boolean / integer variables */
  void Reset();
  /*! \brief Initialize the tree with a single root node */
  void Init(int output_dimension = 1);
  /*! \brief Allocate a new node and return the node's ID */
  int AllocNode();
  /*! \brief Deletes node indexed by node ID */
  void DeleteNode(std::int32_t nid);
  /*! \brief Expand a node based on a numeric split rule */
  void ExpandNode(std::int32_t nid, int split_index, double split_value, bool default_left, double left_value, double right_value);
  /*! \brief Expand a node based on a categorical split rule */
  void ExpandNode(std::int32_t nid, int split_index, std::vector<std::uint32_t> const& categorical_indices, bool default_left, double left_value, double right_value);

  /*!
   * \brief change a non leaf node to a leaf node, delete its children
   * \param nid node id of the node
   * \param value new leaf value
   */
  void ChangeToLeaf(std::int32_t nid, double value) {
    CHECK(this->IsLeaf(this->LeftChild(nid)));
    CHECK(this->IsLeaf(this->RightChild(nid)));
    this->DeleteNode(this->LeftChild(nid));
    this->DeleteNode(this->RightChild(nid));
    this->SetLeaf(nid, value);

    // Add nid to leaves and remove from internal nodes and leaf parents (if it was there)
    leaves_.push_back(nid);
    leaf_parents_.erase(std::remove(leaf_parents_.begin(), leaf_parents_.end(), nid), leaf_parents_.end());
    internal_nodes_.erase(std::remove(internal_nodes_.begin(), internal_nodes_.end(), nid), internal_nodes_.end());
  }
  
  /*!
   * \brief collapse a non leaf node to a leaf node, delete its children
   * \param nid node id of the node
   * \param value new leaf value
   */
  void CollapseToLeaf(std::int32_t nid, double value) {
    if (this->IsLeaf(nid)) return;
    if (this->IsLeaf(this->LeftChild(nid))) {
      CollapseToLeaf(this->LeftChild(nid), 0.0f);
    }
    if (this->IsLeaf(this->RightChild(nid))) {
      CollapseToLeaf(this->RightChild(nid), 0.0f);
    }
    this->ChangeToLeaf(nid, value);
  }
  
  /*!
   * \brief Iterate through all nodes in this tree.
   * \param Function that accepts a node index, and returns false when iteration should
   *        stop, otherwise returns true.
   */
  template <typename Func> void WalkTree(Func func) const {
    std::stack<std::int32_t> nodes;
    nodes.push(kRoot);
    auto &self = *this;
    while (!nodes.empty()) {
      auto nidx = nodes.top();
      nodes.pop();
      if (!func(nidx)) {
        return;
      }
      auto left = self.LeftChild(nidx);
      auto right = self.RightChild(nidx);
      if (left != Tree::kInvalidNodeId) {
        nodes.push(left);
      }
      if (right != Tree::kInvalidNodeId) {
        nodes.push(right);
      }
    }
  }

  /*! \brief Predict a tree based on node membership indices
   * TODO: generalize to vector leaves
   */
  void InplacePredictFromNodes(std::vector<double> result, std::vector<std::int32_t> node_indices);
  std::vector<double> PredictFromNodes(std::vector<std::int32_t> node_indices);

  /** Getters **/
  /*!
   * \brief Whether or not a tree has vector output
   */
  bool HasVectorOutput() const {
    return output_dimension_ > 1;
  }
  
  /*!
   * \brief Index of the node's parent
   * \param nid ID of node being queried
   */
  std::int32_t Parent(std::int32_t nid) const {
    return parent_[nid];
  }
  
  /*!
   * \brief Index of the node's left child
   * \param nid ID of node being queried
   */
  std::int32_t LeftChild(std::int32_t nid) const {
    return cleft_[nid];
  }
  
  /*!
   * \brief Index of the node's right child
   * \param nid ID of node being queried
   */
  std::int32_t RightChild(std::int32_t nid) const {
    return cright_[nid];
  }
  
  /*!
   * \brief Index of the node's "default" child, used when feature is missing
   * \param nid ID of node being queried
   */
  std::int32_t DefaultChild(std::int32_t nid) const {
    return default_left_[nid] ? cleft_[nid] : cright_[nid];
  }
  
  /*!
   * \brief Feature index of the node's split condition
   * \param nid ID of node being queried
   */
  std::int32_t SplitIndex(std::int32_t nid) const {
    return split_index_[nid];
  }
  
  /*!
   * \brief Whether to use the left child node, when the feature in the split condition is missing
   * \param nid ID of node being queried
   */
  bool DefaultLeft(std::int32_t nid) const {
    return default_left_[nid];
  }
  
  /*!
   * \brief Whether the node is leaf node
   * \param nid ID of node being queried
   */
  bool IsLeaf(std::int32_t nid) const {
    return cleft_[nid] == kInvalidNodeId;
  }
  
  /*!
   * \brief Whether the node is root
   * \param nid ID of node being queried
   */
  bool IsRoot(std::int32_t nid) const {
    return parent_[nid] == kInvalidNodeId;
  }
  
  /*!
   * \brief Get leaf value of the leaf node
   * \param nid ID of node being queried
   */
  double LeafValue(std::int32_t nid) const {
    return leaf_value_[nid];
  }
  
  /*!
   * \brief get leaf vector of the leaf node; useful for multi-class random forest classifier
   * \param nid ID of node being queried
   */
  std::vector<double> LeafVector(std::int32_t nid) const {
    std::size_t const offset_begin = leaf_vector_begin_[nid];
    std::size_t const offset_end = leaf_vector_end_[nid];
    if (offset_begin >= leaf_vector_.size() || offset_end > leaf_vector_.size()) {
      // Return empty vector, to indicate the lack of leaf vector
      return std::vector<double>();
    }
    return std::vector<double>(&leaf_vector_[offset_begin], &leaf_vector_[offset_end]);
    // Use unsafe access here, since we may need to take the address of one past the last
    // element, to follow with the range semantic of std::vector<>.
  }
  
  /*!
   * \brief Tests whether the leaf node has a non-empty leaf vector
   * \param nid ID of node being queried
   */
  bool HasLeafVector(std::int32_t nid) const {
    return leaf_vector_begin_[nid] != leaf_vector_end_[nid];
  }

  /*!
   * \brief Get threshold of the node
   * \param nid ID of node being queried
   */
  double Threshold(std::int32_t nid) const {
    return threshold_[nid];
  }

  /*!
   * \brief Get list of all categories belonging to the left/right child node.
   * See the category_list_right_child_ field of each test node to determine whether this list
   * represents the right child node or the left child node. Categories are integers ranging from 0
   * to (n-1), where n is the number of categories in that particular feature. This list is assumed
   * to be in ascending order.
   *
   * \param nid ID of node being queried
   */
  std::vector<std::uint32_t> CategoryList(std::int32_t nid) const {
    std::size_t const offset_begin = category_list_begin_[nid];
    std::size_t const offset_end = category_list_end_[nid];
    if (offset_begin >= category_list_.size() || offset_end > category_list_.size()) {
      // Return empty vector, to indicate the lack of any category list
      // The node might be a numerical split
      return {};
    }
    return std::vector<std::uint32_t>(&category_list_[offset_begin], &category_list_[offset_end]);
    // Use unsafe access here, since we may need to take the address of one past the last
    // element, to follow with the range semantic of std::vector<>.
  }

  /*!
   * \brief Get the type of a node
   * \param nid ID of node being queried
   */
  TreeNodeType NodeType(std::int32_t nid) const {
    return node_type_[nid];
  }

  /*!
   * \brief Query whether this tree contains any categorical splits
   */
  bool HasCategoricalSplit() const {
    return has_categorical_split_;
  }

  /* \brief Count number of leaves in tree. */
  [[nodiscard]] std::int32_t NumLeaves() const;
  [[nodiscard]] std::int32_t NumLeafParents() const;
  [[nodiscard]] std::int32_t NumSplitNodes() const;

  /* \brief Determine whether nid is leaf parent */
  [[nodiscard]] bool IsLeafParent(std::int32_t nid) const {
    // False until we deduce left and right node are
    // available and both are leaves
    bool is_left_leaf = false;
    bool is_right_leaf = false;
    // Check if node nidx is a leaf, if so, return false
    bool is_leaf = this->IsLeaf(nid);
    if (is_leaf){
      return false;
    } else {
      // If nidx is not a leaf, it must have left and right nodes
      // so we check if those are leaves
      std::int32_t left_node = LeftChild(nid);
      std::int32_t right_node = RightChild(nid);
      is_left_leaf = IsLeaf(left_node);
      is_right_leaf = IsLeaf(right_node);
    }
    return is_left_leaf && is_right_leaf;
  }

  /*!
   * \brief Get indices of all internal nodes.
   */
  [[nodiscard]] std::vector<std::int32_t> const& GetInternalNodes() const {
    return internal_nodes_;
  }
  /*!
   * \brief Get indices of all leaf nodes.
   */
  [[nodiscard]] std::vector<std::int32_t> const& GetLeaves() const {
    return leaves_;
  }
  /*!
   * \brief Get indices of all leaf parent nodes.
   */
  [[nodiscard]] std::vector<std::int32_t> const& GetLeafParents() const {
    return leaf_parents_;
  }

  /*!
   * \brief get current depth
   * \param nid node id
   */
  [[nodiscard]] std::int32_t GetDepth(std::int32_t nid) const {
    int depth = 0;
    while (!IsRoot(nid)) {
      ++depth;
      nid = Parent(nid);
    }
    return depth;
  }

  /**
   * \brief Get the total number of nodes including deleted ones in this tree.
   */
  [[nodiscard]] std::int32_t NumNodes() const noexcept { return num_nodes; }
  /**
   * \brief Get the total number of valid nodes in this tree.
   */
  [[nodiscard]] std::int32_t NumValidNodes() const noexcept {
    return num_nodes - num_deleted_nodes;
  }

  /** Setters **/
  /*!
   * \brief Identify left child node
   * \param nid ID of node being modified
   * \param left_child ID of the left child node
   */
  void SetLeftChild(std::int32_t nid, std::int32_t left_child) {
    cleft_[nid] = left_child;
  }

  /*!
   * \brief Identify right child node
   * \param nid ID of node being modified
   * \param right_child ID of the right child node
   */
  void SetRightChild(std::int32_t nid, std::int32_t right_child) {
    cright_[nid] = right_child;
  }

  /*!
   * \brief Identify right child node
   * \param nid ID of node being modified
   * \param right_child ID of the right child node
   */
  void SetParent(std::int32_t child_node, std::int32_t parent_node) {
    parent_[child_node] = parent_node;
  }

  /*!
   * \brief Identify two child nodes of the node and the corresponding parent node of the child nodes
   * \param nid ID of node being modified
   * \param left_child ID of the left child node
   * \param right_child ID of the right child node
   */
  void SetChildren(std::int32_t nid, std::int32_t left_child, std::int32_t right_child) {
    SetLeftChild(nid, left_child);
    SetRightChild(nid, right_child);
  }

  /*!
   * \brief Identify parent node of the left and right node ids
   * \param nid ID of parent node
   * \param left_child ID of the left child node
   * \param right_child ID of the right child node
   */
  void SetParents(std::int32_t nid, std::int32_t left_child, std::int32_t right_child) {
    SetParent(left_child, nid);
    SetParent(right_child, nid);
  }

  /*!
   * \brief Create a numerical test
   * \param nid ID of node being updated
   * \param split_index Feature index to split
   * \param threshold Threshold value
   * \param default_left Default direction when feature is unknown
   */
  void SetNumericSplit(
      std::int32_t nid, std::int32_t split_index, double threshold, bool default_left);
  
  /*!
   * \brief Create a categorical test
   * \param nid ID of node being updated
   * \param split_index Feature index to split
   * \param default_left Default direction when feature is unknown
   * \param category_list List of categories to belong to either the right child node or the left
   *                      child node. Set categories_list_right_child parameter to indicate
   *                      which node the category list should represent.
   */
  void SetCategoricalSplit(std::int32_t nid, std::int32_t split_index, bool default_left,
      std::vector<std::uint32_t> const& category_list);
  
  /*!
   * \brief Set the leaf value of the node
   * \param nid ID of node being updated
   * \param value Leaf value
   */
  void SetLeaf(std::int32_t nid, double value);

  /*!
   * \brief Set the leaf vector of the node; useful for multi-class random forest classifier
   * \param nid ID of node being updated
   * \param leaf_vector Leaf vector
   */
  void SetLeafVector(std::int32_t nid, std::vector<double> const& leaf_vector);

  // Node info
  std::vector<TreeNodeType> node_type_;
  std::vector<std::int32_t> parent_;
  std::vector<std::int32_t> cleft_;
  std::vector<std::int32_t> cright_;
  std::vector<std::int32_t> split_index_;
  std::vector<bool> default_left_;
  std::vector<double> leaf_value_;
  std::vector<double> threshold_;
  std::vector<std::int32_t> internal_nodes_;
  std::vector<std::int32_t> leaves_;
  std::vector<std::int32_t> leaf_parents_;
  std::vector<std::int32_t> deleted_nodes_;
  
  // Leaf vector
  std::vector<double> leaf_vector_;
  std::vector<std::uint64_t> leaf_vector_begin_;
  std::vector<std::uint64_t> leaf_vector_end_;

  // Category list
  std::vector<std::uint32_t> category_list_;
  std::vector<std::uint64_t> category_list_begin_;
  std::vector<std::uint64_t> category_list_end_;

  bool has_categorical_split_{false};
  int output_dimension_{1};
};

} // namespace StochTree

#endif // STOCHTREE_TREE_H_
