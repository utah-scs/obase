#ifndef OBASE_GUIDE_ANNOTATIONS_H
#define OBASE_GUIDE_ANNOTATIONS_H

/*
 * Developer-facing annotation for Pass 2 (guide-converter).
 *
 * Mark a pointer field whose pointee OBASE should manage:
 *
 *     struct Node {
 *         OBASE_GUIDED void *key;
 *         OBASE_GUIDED void *val;
 *         Node *next;               // routing metadata: never annotate
 *     };
 *
 * The converter rewrites the declaration to Guide<T>, repairs casts that
 * no longer compile, and replaces this header's include with Guide.hpp.
 * Until conversion runs, the annotation is a no-op and the code builds
 * and behaves as plain C++.
 *
 * Rules:
 *  - Annotate only authoritative per-record data (keys, values). Routing
 *    metadata (child/next pointers, interior separator keys) must stay raw:
 *    traversals would manufacture hotness and hold the promotion rate above
 *    the pageout threshold.
 *  - One declarator per annotated declaration.
 *  - The pointee must be jemalloc-allocated (jem_malloc/jem_calloc):
 *    migration frees the source through jemalloc and sizes it with
 *    jem_sallocx. This avoids having to do a strlen and expect null termination
 */
#define OBASE_GUIDED [[clang::annotate("obase::guided")]]

#endif // OBASE_GUIDE_ANNOTATIONS_H
