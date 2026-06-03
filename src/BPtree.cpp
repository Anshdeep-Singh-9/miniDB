#include "BPtree.h"
#include "declaration.h"
#include "file_handler.h"
#include <iostream>
#include <algorithm>
#include <cstring>

BPtreeNode::BPtreeNode(uint32_t id, bool leaf)
    : is_leaf(leaf), num_keys(0), page_id(id), next_page_id(INVALID_PAGE_ID), prev_page_id(INVALID_PAGE_ID) {
    // What: initialize one index page as either a leaf node or internal node.
    // Why: B+ Tree pages must start with empty keys, empty children, and invalid links.
    // Example: first INSERT creates a leaf root node with no keys initially.
    for (int i = 0; i < MAX_KEYS; ++i) {
        keys[i] = 0;
        values[i] = RID();
    }
    for (int i = 0; i < MAX_CHILDREN; ++i) {
        children[i] = INVALID_PAGE_ID;
    }
}

void BPtreeNode::serialize(char* buffer) const {
    // What: convert one B+ Tree node into fixed-size page bytes.
    // Why: index nodes are stored in index.dat through DiskManager pages.
    // Example: keys [10, 20] and their RIDs are copied into a 4 KB buffer.
    std::memset(buffer, 0, STORAGE_PAGE_SIZE);
    char* ptr = buffer;
    
    uint32_t leaf_val = is_leaf ? 1 : 0;
    std::memcpy(ptr, &leaf_val, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    std::memcpy(ptr, &num_keys, sizeof(int)); ptr += sizeof(int);
    std::memcpy(ptr, &page_id, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    std::memcpy(ptr, keys, sizeof(int) * MAX_KEYS); ptr += sizeof(int) * MAX_KEYS;
    std::memcpy(ptr, children, sizeof(uint32_t) * MAX_CHILDREN); ptr += sizeof(uint32_t) * MAX_CHILDREN;
    
    for (int i = 0; i < MAX_KEYS; i++) {
        std::memcpy(ptr, &values[i].page_id, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        std::memcpy(ptr, &values[i].slot_id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
    }
    
    std::memcpy(ptr, &next_page_id, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    std::memcpy(ptr, &prev_page_id, sizeof(uint32_t)); ptr += sizeof(uint32_t);
}

void BPtreeNode::deserialize(const char* buffer) {
    // What: rebuild one B+ Tree node object from page bytes.
    // Why: search/insert reads index pages from disk and needs usable C++ fields.
    // Example: page bytes from index.dat become keys, children, and RID values.
    const char* ptr = buffer;
    
    uint32_t leaf_val;
    std::memcpy(&leaf_val, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    is_leaf = (leaf_val == 1);
    
    std::memcpy(&num_keys, ptr, sizeof(int)); ptr += sizeof(int);
    std::memcpy(&page_id, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    std::memcpy(keys, ptr, sizeof(int) * MAX_KEYS); ptr += sizeof(int) * MAX_KEYS;
    std::memcpy(children, ptr, sizeof(uint32_t) * MAX_CHILDREN); ptr += sizeof(uint32_t) * MAX_CHILDREN;
    
    for (int i = 0; i < MAX_KEYS; i++) {
        std::memcpy(&values[i].page_id, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        std::memcpy(&values[i].slot_id, ptr, sizeof(uint16_t)); ptr += sizeof(uint16_t);
    }
    
    std::memcpy(&next_page_id, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
    std::memcpy(&prev_page_id, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
}

BPtree::BPtree(const char* table_name) 
    : root_page_id_(INVALID_PAGE_ID), table_name_(table_name) {
    // What: open the table's index file and load the root-page pointer.
    // Why: every table has its own primary-key index stored as table/<name>/index.dat.
    // Example: BPtree("students") manages table/students/index.dat.
    std::string index_path = table_index_path(table_name_).string();
    disk_manager_ = new DiskManager(index_path);
    if (disk_manager_->open_or_create()) {
        load_root();
    }
}

BPtree::~BPtree() {
    if (disk_manager_) {
        save_root();
        delete disk_manager_;
    }
}

void BPtree::load_root() {
    if (disk_manager_->page_count() == 0) {
        disk_manager_->allocate_page(); // Page 0 for metadata
        root_page_id_ = INVALID_PAGE_ID;
        save_root();
    } else {
        char buffer[STORAGE_PAGE_SIZE];
        disk_manager_->read_page(0, buffer);
        std::memcpy(&root_page_id_, buffer, sizeof(uint32_t));
    }
}

void BPtree::save_root() {
    char buffer[STORAGE_PAGE_SIZE];
    std::memset(buffer, 0, STORAGE_PAGE_SIZE);
    std::memcpy(buffer, &root_page_id_, sizeof(uint32_t));
    disk_manager_->write_page(0, buffer);
}

uint32_t BPtree::allocate_node(bool is_leaf) {
    uint32_t page_id = disk_manager_->allocate_page();
    BPtreeNode node(page_id, is_leaf);
    write_node(node);
    return page_id;
}

void BPtree::read_node(uint32_t page_id, BPtreeNode& node) {
    char buffer[STORAGE_PAGE_SIZE];
    disk_manager_->read_page(page_id, buffer);
    node.deserialize(buffer);
}

void BPtree::write_node(const BPtreeNode& node) {
    char buffer[STORAGE_PAGE_SIZE];
    node.serialize(buffer);
    disk_manager_->write_page(node.page_id, buffer);
}

RID BPtree::search(int key) {
    // What: find the RID stored for a primary-key value.
    // Why: SELECT/UPDATE/DELETE on primary key should jump directly to page and slot.
    // Example: key 5 returns RID(0, 3), meaning row is in data page 0 slot 3.
    if (root_page_id_ == INVALID_PAGE_ID) return RID();
    
    uint32_t curr_id = root_page_id_;
    BPtreeNode curr;
    while (curr_id != INVALID_PAGE_ID) {
        read_node(curr_id, curr);
        if (curr.is_leaf) break;
        int i = 0;
        while (i < curr.num_keys && key >= curr.keys[i]) i++;
        curr_id = curr.children[i];
    }
    
    if (curr_id == INVALID_PAGE_ID) return RID();

    for (int i = 0; i < curr.num_keys; ++i) {
        if (curr.keys[i] == key) return curr.values[i];
    }
    return RID();
}

int BPtree::get_record(int key) {
    RID rid = search(key);
    return (rid.page_id == INVALID_PAGE_ID) ? BPTREE_SEARCH_NOT_FOUND : (int)rid.slot_id;
}

bool BPtree::update_rid(int key, RID new_rid) {
    // What: update the stored row location for an existing primary key.
    // Why: large UPDATE may relocate a tuple to a new slot, so index must point to the new RID.
    // Example: key 5 changes from RID(0, 3) to RID(2, 0).
    if (root_page_id_ == INVALID_PAGE_ID) return false;

    // Traverse to the leaf node that should contain this key
    uint32_t curr_id = root_page_id_;
    BPtreeNode curr;
    while (curr_id != INVALID_PAGE_ID) {
        read_node(curr_id, curr);
        if (curr.is_leaf) break;
        int i = 0;
        while (i < curr.num_keys && key >= curr.keys[i]) i++;
        curr_id = curr.children[i];
    }

    if (curr_id == INVALID_PAGE_ID) return false;

    // Scan the leaf for the exact key and update its RID in place
    for (int i = 0; i < curr.num_keys; ++i) {
        if (curr.keys[i] == key) {
            curr.values[i] = new_rid;
            write_node(curr);  // persist the updated leaf node
            return true;
        }
    }
    return false;  // key not found
}

bool BPtree::remove_key(int key) {
    // What: remove a primary-key entry from the leaf node.
    // Why: DELETE should stop future indexed searches from returning a tombstoned row.
    // Example: DELETE WHERE id = 5 removes key 5 from index.dat.
    if (root_page_id_ == INVALID_PAGE_ID) return false;

    // Traverse to the leaf that should contain this key
    uint32_t curr_id = root_page_id_;
    BPtreeNode curr;
    while (curr_id != INVALID_PAGE_ID) {
        read_node(curr_id, curr);
        if (curr.is_leaf) break;
        int i = 0;
        while (i < curr.num_keys && key >= curr.keys[i]) i++;
        curr_id = curr.children[i];
    }

    if (curr_id == INVALID_PAGE_ID) return false;

    // Find the key in the leaf and remove it by shifting entries left
    for (int i = 0; i < curr.num_keys; ++i) {
        if (curr.keys[i] == key) {
            for (int j = i; j < curr.num_keys - 1; ++j) {
                curr.keys[j]   = curr.keys[j + 1];
                curr.values[j] = curr.values[j + 1];
            }
            curr.num_keys--;
            write_node(curr);
            return true;
        }
    }
    return false;  // key not found in leaf
}

int BPtree::insert_record(int key, int record_num) {
    if (search(key).page_id != INVALID_PAGE_ID) return BPTREE_INSERT_ERROR_EXIST;
    insert(key, RID(0, (uint16_t)record_num));
    return BPTREE_INSERT_SUCCESS;
}

void BPtree::insert(int key, RID rid) {
    // What: insert primary key -> RID into the B+ Tree, splitting nodes if required.
    // Why: the index must stay sorted so future point lookups can navigate quickly.
    // Example: INSERT id=10 at RID(0, 1) stores 10 -> RID(0, 1).
    if (root_page_id_ == INVALID_PAGE_ID) {
        root_page_id_ = allocate_node(true);
        BPtreeNode root_node;
        read_node(root_page_id_, root_node);
        root_node.keys[0] = key;
        root_node.values[0] = rid;
        root_node.num_keys = 1;
        write_node(root_node);
        save_root();
        return;
    }
    
    uint32_t curr_id = root_page_id_;
    BPtreeNode curr;
    while (curr_id != INVALID_PAGE_ID) {
        read_node(curr_id, curr);
        if (curr.is_leaf) break;
        int i = 0;
        while (i < curr.num_keys && key >= curr.keys[i]) i++;
        curr_id = curr.children[i];
    }

    if (curr.num_keys < MAX_KEYS) {
        insert_into_leaf(curr, key, rid);
        write_node(curr);
    } else {
        split_leaf(curr, key, rid);
    }
}

void BPtree::insert_into_leaf(BPtreeNode& leaf, int key, RID rid) {
    int i = leaf.num_keys - 1;
    while (i >= 0 && leaf.keys[i] > key) {
        leaf.keys[i + 1] = leaf.keys[i];
        leaf.values[i + 1] = leaf.values[i];
        i--;
    }
    leaf.keys[i + 1] = key;
    leaf.values[i + 1] = rid;
    leaf.num_keys++;
}

void BPtree::split_leaf(BPtreeNode& leaf, int key, RID rid) {
    // What: split a full leaf into two leaves and push separator key upward.
    // Why: B+ Tree nodes have fixed capacity, so overflow must be handled structurally.
    // Example: inserting a fifth key into a 4-key leaf creates a new right leaf.
    int temp_keys[MAX_KEYS + 1];
    RID temp_values[MAX_KEYS + 1];
    int i = 0;
    while (i < MAX_KEYS && leaf.keys[i] < key) {
        temp_keys[i] = leaf.keys[i];
        temp_values[i] = leaf.values[i];
        i++;
    }
    temp_keys[i] = key;
    temp_values[i] = rid;
    int j = i + 1;
    while (i < MAX_KEYS) {
        temp_keys[j] = leaf.keys[i];
        temp_values[j] = leaf.values[i];
        i++; j++;
    }

    uint32_t new_leaf_id = allocate_node(true);
    BPtreeNode new_leaf;
    read_node(new_leaf_id, new_leaf);
    
    leaf.num_keys = 2;
    for (int k = 0; k < 2; k++) {
        leaf.keys[k] = temp_keys[k];
        leaf.values[k] = temp_values[k];
    }
    new_leaf.num_keys = 3;
    for (int k = 0; k < 3; k++) {
        new_leaf.keys[k] = temp_keys[k + 2];
        new_leaf.values[k] = temp_values[k + 2];
    }

    new_leaf.next_page_id = leaf.next_page_id;
    if (leaf.next_page_id != INVALID_PAGE_ID) {
        BPtreeNode next_node;
        read_node(leaf.next_page_id, next_node);
        next_node.prev_page_id = new_leaf_id;
        write_node(next_node);
    }
    leaf.next_page_id = new_leaf_id;
    new_leaf.prev_page_id = leaf.page_id;

    write_node(leaf);
    write_node(new_leaf);
    insert_into_parent(leaf.page_id, new_leaf.keys[0], new_leaf_id);
}

void BPtree::insert_into_parent(uint32_t old_node_id, int key, uint32_t new_node_id) {
    if (root_page_id_ == old_node_id) {
        uint32_t new_root_id = allocate_node(false);
        BPtreeNode new_root;
        read_node(new_root_id, new_root);
        new_root.keys[0] = key;
        new_root.children[0] = old_node_id;
        new_root.children[1] = new_node_id;
        new_root.num_keys = 1;
        write_node(new_root);
        root_page_id_ = new_root_id;
        save_root();
        return;
    }

    uint32_t curr_id = root_page_id_;
    uint32_t parent_id = INVALID_PAGE_ID;
    while (curr_id != INVALID_PAGE_ID) {
        BPtreeNode curr;
        read_node(curr_id, curr);
        if (curr.is_leaf) break;
        bool found = false;
        for (int i = 0; i <= curr.num_keys; i++) {
            if (curr.children[i] == old_node_id) {
                parent_id = curr_id;
                found = true; break;
            }
        }
        if (found) break;
        int i = 0;
        while (i < curr.num_keys && key >= curr.keys[i]) i++;
        curr_id = curr.children[i];
    }

    BPtreeNode parent;
    read_node(parent_id, parent);
    if (parent.num_keys < MAX_KEYS) {
        int i = parent.num_keys - 1;
        while (i >= 0 && parent.keys[i] > key) {
            parent.keys[i + 1] = parent.keys[i];
            parent.children[i + 2] = parent.children[i + 1];
            i--;
        }
        parent.keys[i + 1] = key;
        parent.children[i + 2] = new_node_id;
        parent.num_keys++;
        write_node(parent);
    } else {
        split_internal(parent, key, new_node_id);
    }
}

void BPtree::split_internal(BPtreeNode& parent, int key, uint32_t new_node_id) {
    int temp_keys[MAX_KEYS + 1];
    uint32_t temp_children[MAX_CHILDREN + 1];
    int i = 0;
    while (i < MAX_KEYS && parent.keys[i] < key) {
        temp_keys[i] = parent.keys[i];
        temp_children[i] = parent.children[i];
        i++;
    }
    temp_keys[i] = key;
    temp_children[i] = parent.children[i];
    temp_children[i+1] = new_node_id;
    int j = i + 1;
    while (i < MAX_KEYS) {
        temp_keys[j] = parent.keys[i];
        temp_children[j+1] = parent.children[i+1];
        i++; j++;
    }

    uint32_t new_int_id = allocate_node(false);
    BPtreeNode new_int;
    read_node(new_int_id, new_int);
    parent.num_keys = 2;
    for (int k = 0; k < 2; k++) {
        parent.keys[k] = temp_keys[k];
        parent.children[k] = temp_children[k];
    }
    parent.children[2] = temp_children[2];
    int push_up = temp_keys[2];
    new_int.num_keys = 2;
    for (int k = 0; k < 2; k++) {
        new_int.keys[k] = temp_keys[k + 3];
        new_int.children[k] = temp_children[k + 3];
    }
    new_int.children[2] = temp_children[5];
    write_node(parent);
    write_node(new_int);
    insert_into_parent(parent.page_id, push_up, new_int_id);
}

void BPtree::print() {
    if (root_page_id_ == INVALID_PAGE_ID) {
        std::cout << "Tree is empty" << std::endl;
        return;
    }
    print_node(root_page_id_, 0);
}

void BPtree::print_node(uint32_t page_id, int level) {
    BPtreeNode node;
    read_node(page_id, node);
    for (int i = 0; i < level; i++) std::cout << "  ";
    std::cout << (node.is_leaf ? "[L] " : "[I] ") << "P" << node.page_id << ": ";
    for (int i = 0; i < node.num_keys; i++) {
        std::cout << node.keys[i] << (i == node.num_keys - 1 ? "" : ", ");
    }
    std::cout << std::endl;
    if (!node.is_leaf) {
        for (int i = 0; i <= node.num_keys; i++) {
            print_node(node.children[i], level + 1);
        }
    }
}
