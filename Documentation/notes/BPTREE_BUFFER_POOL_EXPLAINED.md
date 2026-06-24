# B+ Tree and Buffer Pool Explained

This note continues after:

```text
main.cpp
parser.cpp
create.cpp
insert.cpp
tuple_serializer.cpp
data_page.cpp
disk_manager.cpp
```

The next two important units are:

```text
1. BPtree.cpp / BPtree.h
2. buffer_pool_manager.cpp / buffer_pool_manager.h
```

The high-level storage chain is:

```text
TupleSerializer
  = row values -> tuple bytes

DataPage
  = tuple bytes -> slot inside 4KB page

DiskManager
  = 4KB page -> exact position inside data.dat or index.dat

B+ Tree
  = primary key -> RID(page_id, slot_id)

BufferPoolManager
  = keeps frequently used pages in RAM
```

---

# Unit 1: B+ Tree

Files:

```text
include/BPtree.h
src/BPtree.cpp
```

Important correction:

```text
This is a B+ Tree, not B++ Tree.
```

---

## Why B+ Tree Exists

Without an index, this query:

```sql
SELECT * FROM students WHERE id = 10;
```

would require a full table scan:

```text
page 0 slot 0
page 0 slot 1
page 0 slot 2
page 1 slot 0
page 1 slot 1
...
```

That means MiniDB would inspect every row until it finds the row with `id = 10`.

B+ Tree avoids this by storing:

```text
primary_key -> RID(page_id, slot_id)
```

Example:

```text
10 -> RID(0, 3)
25 -> RID(2, 1)
40 -> RID(5, 0)
```

So if the query is:

```sql
SELECT * FROM students WHERE id = 25;
```

B+ Tree returns:

```text
RID(page_id=2, slot_id=1)
```

Then MiniDB can directly read:

```text
data.dat page 2 slot 1
```

No full table scan is needed.

---

## Data File vs Index File

Actual table rows are stored in:

```text
table/students/data.dat
```

The primary-key index is stored in:

```text
table/students/index.dat
```

Meaning:

```text
data.dat
  stores actual tuple/page bytes

index.dat
  stores B+ Tree nodes
```

Example row:

```text
id = 1, name = Aryan, dept = CSE
```

Suppose DataPage stores this row at:

```text
RID(0, 2)
```

Then B+ Tree stores:

```text
1 -> RID(0, 2)
```

So the index does not store the full row.

It stores only:

```text
primary key
row location
```

---

## Core Struct: BPtreeNode

In `BPtree.h`:

```cpp
struct BPtreeNode {
    bool is_leaf;
    int num_keys;
    uint32_t page_id;
    int keys[MAX_KEYS];

    uint32_t children[MAX_CHILDREN];

    RID values[MAX_KEYS];
    uint32_t next_page_id;
    uint32_t prev_page_id;
};
```

Constants:

```cpp
MAX_KEYS = 4
MAX_CHILDREN = 5
```

So one B+ Tree node can store at most:

```text
4 keys
5 child pointers
```

That makes it an order-5 B+ Tree.

---

## Leaf Node

A leaf node stores actual key-to-RID mappings.

Example:

```text
Leaf page 2:

keys:
[10, 20, 30]

values:
[RID(0,1), RID(0,4), RID(2,0)]
```

Meaning:

```text
key 10 -> row is in data page 0 slot 1
key 20 -> row is in data page 0 slot 4
key 30 -> row is in data page 2 slot 0
```

Leaf nodes contain the final answer for a search.

---

## Internal Node

An internal node does not store row locations.

It stores routing keys and child page ids.

Example:

```text
Internal page 5:

keys:
[30, 60]

children:
[page 1, page 2, page 3]
```

Meaning:

```text
key < 30        -> go to child page 1
30 <= key < 60  -> go to child page 2
key >= 60       -> go to child page 3
```

Internal nodes are like direction boards.

Leaf nodes contain actual `primary_key -> RID` entries.

---

## Main B+ Tree Rule

In a B+ Tree:

```text
Actual row pointers exist only in leaf nodes.
Internal nodes only guide the search.
```

Your project follows this:

```cpp
// Internal nodes
uint32_t children[MAX_CHILDREN];

// Leaf nodes
RID values[MAX_KEYS];
```

---

## Constructor

```cpp
BPtree::BPtree(const char* table_name)
```

Example:

```cpp
BPtree index("students");
```

This creates:

```text
index_path = table/students/index.dat
disk_manager_ = DiskManager(index_path)
open_or_create index.dat
load_root()
```

So every table has its own index file:

```text
students -> table/students/index.dat
courses  -> table/courses/index.dat
```

---

## Page 0 Metadata

In `index.dat`, page 0 is special.

It stores the root page id.

```cpp
disk_manager_->allocate_page(); // Page 0 for metadata
```

So layout is:

```text
index.dat

page 0 -> metadata/root pointer
page 1 -> B+ Tree node
page 2 -> B+ Tree node
page 3 -> B+ Tree node
...
```

Example:

```text
page 0 contains root_page_id = 1
```

Meaning:

```text
the root node is stored in index page 1
```

---

## load_root()

```cpp
void BPtree::load_root()
```

If `index.dat` is empty:

```text
allocate page 0
root_page_id_ = INVALID_PAGE_ID
save_root()
```

Meaning:

```text
the index file exists, but the tree has no root node yet
```

If `index.dat` already exists:

```text
read page 0
extract root_page_id_
```

Example:

```text
page 0 stores root_page_id = 3
```

Then:

```text
root_page_id_ = 3
```

---

## save_root()

```cpp
void BPtree::save_root()
```

This writes the current root page id into page 0.

Example:

```text
root_page_id_ = 5
```

Then page 0 stores:

```text
5
```

Why?

Because after program restart, MiniDB must know where the B+ Tree root is.

---

## allocate_node()

```cpp
uint32_t BPtree::allocate_node(bool is_leaf)
```

Use:

```text
create a new B+ Tree node page inside index.dat
```

Example:

```cpp
allocate_node(true)
```

Means:

```text
allocate new page in index.dat
initialize it as a leaf node
write node to disk
return page_id
```

If `index.dat` currently has only page 0:

```text
new node page_id = 1
```

Then:

```text
page 1 contains an empty leaf node
```

---

## Node Serialization

```cpp
void BPtreeNode::serialize(char* buffer) const
```

This converts a B+ Tree node into raw 4096-byte page data.

It writes:

```text
is_leaf
num_keys
page_id
keys[]
children[]
values[]
next_page_id
prev_page_id
```

Example node:

```text
Leaf page 1:
keys = [10, 20]
values = [RID(0,1), RID(0,4)]
```

After serialization:

```text
raw 4096-byte page stored in index.dat
```

---

## Node Deserialization

```cpp
void BPtreeNode::deserialize(const char* buffer)
```

Reverse process:

```text
raw bytes from index.dat
    ↓
BPtreeNode object
```

It rebuilds:

```text
is_leaf
num_keys
page_id
keys
children
values
leaf links
```

This is similar to tuple serialization, but for index nodes.

---

## read_node()

```cpp
void BPtree::read_node(uint32_t page_id, BPtreeNode& node)
```

Flow:

```text
DiskManager reads 4096 bytes from index.dat
node.deserialize(buffer)
```

So:

```text
index.dat page 2
    ↓
raw bytes
    ↓
BPtreeNode
```

---

## write_node()

```cpp
void BPtree::write_node(const BPtreeNode& node)
```

Flow:

```text
node.serialize(buffer)
DiskManager writes 4096 bytes into index.dat
```

So:

```text
BPtreeNode object
    ↓
raw bytes
    ↓
index.dat page
```

---

## search()

```cpp
RID BPtree::search(int key)
```

Use:

```text
find primary key and return its RID
```

Example:

```cpp
index.search(25)
```

Returns:

```text
RID(2, 1)
```

Meaning:

```text
row with primary key 25 is in data page 2 slot 1
```

---

## search() Dry Run

Suppose tree:

```text
Root internal:
keys = [30, 60]
children = [page 1, page 2, page 3]

Leaf page 1:
[10, 20]

Leaf page 2:
[30, 40, 50]

Leaf page 3:
[60, 70]
```

Search:

```cpp
search(40)
```

Start at root:

```text
key = 40
root keys = [30, 60]
```

Code:

```cpp
while (i < curr.num_keys && key >= curr.keys[i]) i++;
```

For key `40`:

```text
40 >= 30 -> yes, i = 1
40 >= 60 -> no
```

So choose:

```text
children[1] = page 2
```

Now read leaf page 2:

```text
[30, 40, 50]
```

Scan:

```text
30 != 40
40 == 40
return RID for 40
```

Final result:

```text
RID(page_id, slot_id)
```

---

## insert()

```cpp
void BPtree::insert(int key, RID rid)
```

Use:

```text
insert primary_key -> row location
```

Example:

```cpp
index.insert(10, RID(0, 2));
```

Means:

```text
primary key 10 is located in data page 0 slot 2
```

---

## First Insert

If tree is empty:

```cpp
if (root_page_id_ == INVALID_PAGE_ID)
```

Then:

```text
allocate new leaf node
make it root
insert key into root
save root page id
```

Example:

```cpp
insert(10, RID(0, 1))
```

Before:

```text
root_page_id = INVALID_PAGE_ID
```

After:

```text
index.dat page 0 -> root_page_id = 1

page 1 leaf:
keys = [10]
values = [RID(0,1)]
```

Tree:

```text
[L] page 1: 10
```

---

## insert_into_leaf()

```cpp
void BPtree::insert_into_leaf(BPtreeNode& leaf, int key, RID rid)
```

Use:

```text
insert key in sorted order inside a leaf
```

Example leaf:

```text
[10, 30, 40]
```

Insert:

```text
20
```

Process:

```text
40 shifts right
30 shifts right
20 goes between 10 and 30
```

After:

```text
[10, 20, 30, 40]
```

Why sorted?

Because B+ Tree search depends on ordered keys.

---

## split_leaf()

Because `MAX_KEYS = 4`, a leaf can hold only 4 keys.

If leaf already has:

```text
[10, 20, 30, 40]
```

and we insert:

```text
25
```

Temporary keys become:

```text
[10, 20, 25, 30, 40]
```

But 5 keys cannot fit in one node.

So `split_leaf()` runs.

Your implementation divides 5 keys like this:

```text
old leaf gets first 2 keys
new leaf gets last 3 keys
```

After split:

```text
old leaf:
[10, 20]

new leaf:
[25, 30, 40]
```

Then it pushes this separator key upward:

```text
new_leaf.keys[0] = 25
```

Meaning:

```text
keys >= 25 go to the new right leaf
```

---

## Leaf Links

B+ Tree leaves are connected using:

```cpp
next_page_id
prev_page_id
```

Example:

```text
leaf page 1 <-> leaf page 2 <-> leaf page 3
```

This is useful for range scans in real DBMS systems.

Your current project maintains these links during leaf split:

```text
new_leaf.next_page_id = old leaf's next
old leaf.next_page_id = new leaf
new_leaf.prev_page_id = old leaf
```

So after split:

```text
old leaf <-> new leaf
```

---

## insert_into_parent()

```cpp
void BPtree::insert_into_parent(uint32_t old_node_id, int key, uint32_t new_node_id)
```

Use:

```text
after a split, tell the parent about the new child
```

Example:

```text
old leaf page 1: [10, 20]
new leaf page 2: [25, 30, 40]
separator key = 25
```

Parent must store:

```text
key 25 separates page 1 and page 2
```

---

## If Old Node Was Root

If root itself split:

```cpp
if (root_page_id_ == old_node_id)
```

Then create a new root.

Before:

```text
root leaf page 1:
[10, 20, 25, 30, 40] overflow
```

After:

```text
new root page 3:
keys = [25]
children = [page 1, page 2]

page 1 leaf:
[10, 20]

page 2 leaf:
[25, 30, 40]
```

Tree:

```text
        [25]
       /    \
[10,20]    [25,30,40]
```

This is how B+ Tree height grows.

---

## split_internal()

Internal nodes also have max 4 keys.

If internal node overflows, it splits too.

Example temporary internal keys:

```text
[20, 40, 60, 80, 100]
```

Your code:

```text
left internal gets first 2 keys
middle key is pushed up
right internal gets last 2 keys
```

Result:

```text
left internal:
[20, 40]

push up:
60

right internal:
[80, 100]
```

Parent receives:

```text
60
```

This is normal B+ Tree internal-node splitting.

---

## update_rid()

```cpp
bool BPtree::update_rid(int key, RID new_rid)
```

Use:

```text
same primary key, but row moved to another page/slot
```

Example before:

```text
10 -> RID(0, 2)
```

After update, tuple becomes larger and cannot fit in old slot, so row moves:

```text
10 -> RID(3, 0)
```

Then:

```cpp
index.update_rid(10, RID(3, 0));
```

Important:

```text
primary key does not change
only row location changes
```

This is why primary key updates are restricted in the project.

---

## remove_key()

```cpp
bool BPtree::remove_key(int key)
```

Use:

```text
DELETE should remove index entry
```

Example before:

```text
leaf:
[10, 20, 30]
```

Delete key `20`:

```text
[10, 30]
```

The code shifts entries left.

Important limitation:

```text
Your code removes the key from the leaf,
but does not rebalance or merge underfull nodes.
```

A production B+ Tree would also handle:

```text
borrow from sibling
merge leaf
update parent separator
shrink root
```

For an educational DBMS project, this simpler delete is acceptable.

---

## B+ Tree Insert Full Dry Run

Query:

```sql
INSERT INTO students VALUES (25, "Aryan", "CSE");
```

Assume tuple is stored by DataPage at:

```text
RID(0, 3)
```

Then `insert.cpp` does:

```cpp
index.insert(25, RID(0, 3));
```

B+ Tree stores:

```text
25 -> RID(0, 3)
```

Later query:

```sql
SELECT * FROM students WHERE id = 25;
```

Flow:

```text
BPtree.search(25)
    ↓
returns RID(0, 3)

DataPage reads page 0 slot 3
    ↓
TupleSerializer deserializes tuple bytes
    ↓
row is printed
```

---

## One-Line Understanding

`BPtree.cpp` is MiniDB's primary-key index. It stores sorted mappings from `primary_key` to `RID(page_id, slot_id)` inside `index.dat`, so SELECT, UPDATE, and DELETE by primary key can jump directly to the row instead of scanning the full table.

---

# Unit 2: BufferPoolManager

Files:

```text
include/buffer_pool_manager.h
src/buffer_pool_manager.cpp
```

---

## Why Buffer Pool Exists

DiskManager can read and write pages.

But if every operation directly uses DiskManager:

```text
read page 0 from disk
read page 0 from disk again
read page 0 from disk again
```

That is wasteful.

BufferPoolManager keeps disk pages in RAM.

```text
DiskManager
  = knows how to read/write pages from data.dat

BufferPoolManager
  = knows which pages are already cached in RAM
```

---

## Main Concept

Buffer pool is an array of memory frames.

```text
Frame 0
Frame 1
Frame 2
...
```

Each frame can hold one disk page.

Example:

```text
Frame 0 -> page_id 5
Frame 1 -> page_id 2
Frame 2 -> page_id 9
```

Meaning:

```text
page_id  = disk page number
frame_id = RAM slot number
```

---

## BufferFrame

```cpp
struct BufferFrame {
    uint32_t frame_id;
    uint32_t page_id;
    bool is_dirty;
    uint32_t pin_count;
    bool is_valid;
    std::vector<char> page_data;
};
```

---

## frame_id

RAM frame number.

Example:

```text
frame_id = 0
```

Means:

```text
first slot in buffer pool
```

---

## page_id

Which disk page is currently loaded in this frame.

Example:

```text
frame 0 contains disk page 3
```

Then:

```text
frame_id = 0
page_id = 3
```

---

## is_dirty

Dirty means:

```text
RAM page has changes not yet written to disk
```

Example:

```text
insert tuple into page 3 in RAM
```

Now:

```text
is_dirty = true
```

Because:

```text
RAM page 3 is newer than data.dat page 3
```

---

## pin_count

Pin count means:

```text
how many users are currently using this page
```

If:

```text
pin_count > 0
```

then page cannot be evicted.

Why?

Because some operation still holds a pointer to it.

Example:

```cpp
char* page = bpm.fetch_page(3);
```

Now:

```text
pin_count = 1
```

After operation finishes:

```cpp
bpm.unpin_page(3, true);
```

Now:

```text
pin_count = 0
```

Now it can be evicted later.

---

## is_valid

Means the frame currently contains a real page.

At start:

```text
is_valid = false
```

After loading page:

```text
is_valid = true
```

---

## page_data

```cpp
std::vector<char> page_data;
```

This is the actual 4096-byte page in RAM.

Example:

```text
page_data = copy of data.dat page 2
```

DataPage can operate on this memory.

---

## Constructor

```cpp
BufferPoolManager::BufferPoolManager(std::size_t pool_size,
                                     DiskManager* disk_manager)
```

Example:

```cpp
BufferPoolManager bpm(3, &disk);
```

Creates:

```text
3 RAM frames
all empty
page_table empty
LRU list empty
```

Initial state:

```text
Frame 0: invalid
Frame 1: invalid
Frame 2: invalid
```

---

## Page Table

```cpp
std::unordered_map<uint32_t, uint32_t> page_table_;
```

Maps:

```text
page_id -> frame_id
```

Example:

```text
page_table_[5] = 0
page_table_[2] = 1
```

Means:

```text
disk page 5 is in RAM frame 0
disk page 2 is in RAM frame 1
```

This lets BufferPoolManager quickly answer:

```text
Is page 5 already in RAM?
```

---

## LRU List

```cpp
std::list<uint32_t> lru_list_;
```

Stores frame ids ordered by usage.

Your code uses:

```text
front = least recently used
back  = most recently used
```

When frame is touched:

```cpp
touch_lru(frame_id)
```

It removes the frame id from list and pushes it to the back.

Example:

```text
LRU list: [0, 1, 2]
```

Means:

```text
frame 0 is oldest
frame 2 is newest
```

If eviction is needed, try frame 0 first.

---

## fetch_page()

```cpp
char* BufferPoolManager::fetch_page(uint32_t page_id)
```

Use:

```text
get page into RAM and return pointer to its bytes
```

This is the main function.

---

## fetch_page(): Cache Hit

If page is already in RAM:

```cpp
if (hit != page_table_.end())
```

Example:

```text
page_table_[3] = 1
```

Meaning:

```text
page 3 is already in frame 1
```

Then:

```text
frame.pin_count++
stats_.cache_hits++
touch_lru(frame)
return page_data pointer
```

Before:

```text
Frame 1:
page_id = 3
pin_count = 0
```

After:

```text
Frame 1:
page_id = 3
pin_count = 1
```

No disk read is needed.

That is the main benefit of buffer pool.

---

## fetch_page(): Cache Miss

If page is not in RAM:

```text
need to load it from disk
```

Steps:

```text
find free frame
or pick victim frame
read page from DiskManager
set pin_count = 1
add page_id -> frame_id in page_table
return page_data pointer
```

Example:

```cpp
fetch_page(7)
```

If frame 0 is free:

```text
DiskManager reads page 7 from data.dat
Frame 0 stores page 7
page_table_[7] = 0
pin_count = 1
```

---

## new_page()

```cpp
char* BufferPoolManager::new_page(uint32_t& page_id_out)
```

Use:

```text
allocate a brand new disk page and keep it in RAM
```

Example:

```cpp
uint32_t page_id;
char* page = bpm.new_page(page_id);
```

Flow:

```text
find free/victim frame
DiskManager.allocate_page()
clear frame memory to zero
mark dirty
pin_count = 1
return page pointer
```

Why dirty immediately?

Because the new RAM page is initialized and should eventually be written to disk.

Example:

```text
new page_id = 4

Frame 0:
page_id = 4
is_dirty = true
pin_count = 1
page_data = all zero bytes
```

Then upper layer may call DataPage functions on that page.

---

## unpin_page()

```cpp
bool BufferPoolManager::unpin_page(uint32_t page_id, bool is_dirty)
```

Use:

```text
release page after operation is done
```

Example:

```cpp
bpm.unpin_page(3, true);
```

Meaning:

```text
I am done using page 3.
Also, I modified it.
```

Before:

```text
page 3 pin_count = 1
is_dirty = false
```

After:

```text
pin_count = 0
is_dirty = true
```

Now page can be evicted later.

Important:

```text
If pages are fetched but never unpinned,
pin_count stays > 0,
and buffer pool eventually gets stuck.
```

---

## Dirty Page

Dirty page means:

```text
RAM copy changed
disk copy is old
```

Example:

```text
data.dat page 2:
does not have new tuple yet

RAM frame page 2:
has new tuple
```

So:

```text
is_dirty = true
```

Before eviction or shutdown:

```text
write RAM page to disk
```

---

## flush_page()

```cpp
bool BufferPoolManager::flush_page(uint32_t page_id)
```

Use:

```text
write one dirty cached page to disk immediately
```

Example:

```cpp
flush_page(2)
```

If page 2 is dirty:

```text
DiskManager.write_page(2, page_data)
is_dirty = false
```

After flush:

```text
RAM and disk match
```

---

## flush_all_pages()

```cpp
void BufferPoolManager::flush_all_pages()
```

Use:

```text
write every dirty page to disk
```

Destructor calls this:

```cpp
BufferPoolManager::~BufferPoolManager() {
    flush_all_pages();
}
```

So before BufferPoolManager is destroyed, it tries to persist dirty pages.

---

## pick_victim_frame()

```cpp
uint32_t BufferPoolManager::pick_victim_frame()
```

Use:

```text
choose a frame to evict when RAM pool is full
```

Rules:

```text
use LRU order
skip pinned frames
if dirty, flush before removing
```

Example:

```text
Buffer pool size = 3

Frame 0: page 1, pin_count 0, dirty false
Frame 1: page 2, pin_count 1, dirty false
Frame 2: page 3, pin_count 0, dirty true

LRU list: [0, 1, 2]
```

Need space for page 4.

Start from LRU front:

```text
frame 0 pin_count = 0
choose frame 0
```

Frame 0 is clean, so no write is needed.

Evict:

```text
remove page_table_[1]
clear frame
return frame_id 0
```

Now page 4 can be loaded into frame 0.

---

## Dirty Victim

If victim is dirty:

```text
Frame 2: page 3 dirty true
```

Before eviction:

```text
DiskManager.write_page(3, frame.page_data)
is_dirty = false
```

Then remove it.

Why?

Because otherwise changes would be lost.

---

## Pinned Victim

If frame is pinned:

```text
pin_count > 0
```

It cannot be evicted.

Because someone is still using its pointer.

If all frames are pinned:

```text
pick_victim_frame returns INVALID_FRAME_ID
fetch_page/new_page fails
```

---

## load_page_into_frame()

```cpp
bool BufferPoolManager::load_page_into_frame(uint32_t page_id, uint32_t frame_id)
```

Use:

```text
read disk page into selected RAM frame
```

Steps:

```text
if frame currently has dirty page, flush it
erase old page_table mapping
DiskManager.read_page(page_id)
set frame metadata
page_table_[page_id] = frame_id
```

Example:

```text
load page 8 into frame 0
```

After:

```text
Frame 0:
page_id = 8
is_valid = true
is_dirty = false
pin_count = 0
page_data = bytes from data.dat page 8
```

Then `fetch_page()` sets:

```text
pin_count = 1
```

---

## checkpoint()

```cpp
std::size_t BufferPoolManager::checkpoint(std::size_t max_pages)
```

Use:

```text
flush some dirty unpinned pages proactively
```

Example:

```cpp
checkpoint(1)
```

Flushes at most one dirty unpinned page.

Why useful?

So dirty pages do not accumulate until shutdown or eviction.

Your code also triggers a small background checkpoint when:

```cpp
dirty_page_count() > pool_size_ / 2
```

Meaning:

```text
if more than half the buffer pool is dirty,
flush one old dirty unpinned page
```

---

## Buffer Pool Full Dry Run

Assume:

```text
pool_size = 2
```

Initial:

```text
Frame 0 empty
Frame 1 empty
page_table empty
```

Step 1:

```cpp
fetch_page(0)
```

Cache miss:

```text
read page 0 from disk
put in frame 0
pin_count = 1
page_table[0] = 0
LRU = [0]
```

Step 2:

```cpp
unpin_page(0, false)
```

Now:

```text
Frame 0:
page 0
pin_count = 0
dirty = false
```

Step 3:

```cpp
fetch_page(1)
```

Cache miss:

```text
put page 1 in frame 1
pin_count = 1
page_table[1] = 1
LRU = [0, 1]
```

Step 4:

```cpp
unpin_page(1, true)
```

Now:

```text
Frame 1:
page 1
pin_count = 0
dirty = true
```

Step 5:

```cpp
fetch_page(2)
```

Pool is full.

Need victim.

LRU:

```text
[0, 1]
```

Frame 0 is least recently used and unpinned.

Frame 0 is clean, so evict directly:

```text
remove page 0
read page 2 into frame 0
page_table[2] = 0
```

Final:

```text
Frame 0 -> page 2
Frame 1 -> page 1 dirty
```

---

## Relation: DataPage and BufferPool

BufferPoolManager does not understand slots.

It returns:

```cpp
char* page_data
```

Then DataPage understands that memory.

Example:

```cpp
char* page = bpm.fetch_page(page_id);
DataPage data_page(page);
data_page.insert_tuple(...);
bpm.unpin_page(page_id, true);
```

Meaning:

```text
BufferPool gives raw 4096 bytes in RAM.
DataPage understands the layout inside those bytes.
unpin_page(..., true) tells BufferPool page changed.
```

---

## Relation: DiskManager and BufferPool

DiskManager:

```text
read/write from actual file
```

BufferPoolManager:

```text
decides whether disk read/write is needed
```

Cache hit:

```text
fetch_page(3)
page already in RAM
DiskManager is not called
```

Cache miss:

```text
fetch_page(3)
page not in RAM
DiskManager.read_page(3)
```

Dirty eviction:

```text
evict page 5
DiskManager.write_page(5)
```

---

## B+ Tree vs Buffer Pool

B+ Tree answers:

```text
Where is the row with primary key 25?
```

Returns:

```text
RID(2, 1)
```

BufferPoolManager answers:

```text
Is page 2 already in RAM?
If not, load it.
```

Returns:

```text
char* pointer to page 2 bytes
```

So:

```text
B+ Tree = search/index structure
Buffer Pool = RAM cache for pages
```

They solve different problems.

---

## Complete Flow: SELECT by Primary Key

Query:

```sql
SELECT * FROM students WHERE id = 25;
```

Flow:

```text
BPtree.search(25)
    ↓
returns RID(page_id=2, slot_id=1)

BufferPoolManager.fetch_page(2)
    ↓
gets page 2 into RAM

DataPage.read_tuple(slot_id=1)
    ↓
extracts tuple bytes

TupleSerializer.deserialize()
    ↓
converts bytes into readable values
```

---

## Complete Flow: INSERT

Query:

```sql
INSERT INTO students VALUES (25, "Aryan", "CSE");
```

Flow:

```text
TupleSerializer
  values -> tuple bytes

BufferPoolManager
  fetches or creates a data page in RAM

DataPage
  inserts tuple bytes into a slot
  returns slot_id

RID is formed
  RID(page_id, slot_id)

B+ Tree
  stores primary_key -> RID

BufferPoolManager
  page marked dirty

DiskManager
  eventually writes full 4KB page to data.dat
```

---

## Final One-Line Understanding

`BPtree.cpp` helps MiniDB find rows fast using `primary_key -> RID`, while `buffer_pool_manager.cpp` helps MiniDB access pages fast by caching disk pages in RAM and writing dirty pages back only when needed.

