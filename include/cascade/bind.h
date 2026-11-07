/* cascade — data binding: a model of named values (and lists of models) that the page shows and
 * edits through data-* attributes, kept in step both ways without code per element. */
#ifndef CASCADE_BIND_H
#define CASCADE_BIND_H

#include "cascade/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdView CscdView;
typedef struct CscdElement CscdElement;
typedef struct CscdModel CscdModel;

/* A model: values by name (text; numbers and booleans are kept as their text) and lists of models
 * by name. Shared (cscdRelease); the lists' items belong to their model. */
CscdModel* cscdModelNew(void);
void cscdModelSet(CscdModel* m, CscdSv name, CscdSv text);
void cscdModelSetNumber(CscdModel* m, CscdSv name, double value);
void cscdModelSetBool(CscdModel* m, CscdSv name, bool value);
CscdSv cscdModelGet(const CscdModel* m, CscdSv name); /* "" when unset; good until it is set again */
double cscdModelNumber(const CscdModel* m, CscdSv name);
/* truthy: set, and none of "", "0", "false" */
bool cscdModelBool(const CscdModel* m, CscdSv name);

/* Lists: items are models too (what an item of data-each binds to). */
CscdModel* cscdModelListAdd(CscdModel* m, CscdSv list); /* a new item, at the end */
CscdModel* cscdModelListInsert(CscdModel* m, CscdSv list, size_t at);
void cscdModelListRemove(CscdModel* m, CscdSv list, size_t at);
void cscdModelListMove(CscdModel* m, CscdSv list, size_t from, size_t to);
void cscdModelListClear(CscdModel* m, CscdSv list);
size_t cscdModelListCount(const CscdModel* m, CscdSv list);
CscdModel* cscdModelListItem(const CscdModel* m, CscdSv list, size_t at); /* NULL past the end */
/* The model a list item is in (NULL for one made with cscdModelNew). */
CscdModel* cscdModelParent(const CscdModel* m);

/* Called when the page changed a value (a field bound with data-value or data-checked): the model
 * holding it (an item, for a field inside data-each) and its name. */
typedef void (*CscdModelFn)(void* user, CscdModel* m, CscdSv name);
void cscdModelOnChange(CscdModel* m, CscdModelFn fn, void* user);

/* The view shows the model in root and what it holds (NULL: the whole document), through these
 * attributes (a name may start with "!": the test turned around):
 *   data-text="name"         the element's text
 *   data-value="name"        a field's value, both ways (what is typed sets the model)
 *   data-checked="name"      a checkbox's or a radio's state, both ways
 *   data-show="name"         shown when the value is truthy (display: none when not); data-hide
 *   data-class-x="name"      class x while the value is truthy
 *   data-attr-x="name"       attribute x set to the value (removed when it is "")
 *   data-each="list"         the first <template> it holds, repeated for each item of the list,
 *                            what is inside each copy bound to its item
 * The page follows the model at the view's next update: only what changed is touched (a list keeps
 * the copies of the items it still has, in their new order). */
void cscdViewBind(CscdView* v, CscdElement* root, CscdModel* m);
void cscdViewUnbind(CscdView* v, CscdElement* root);
/* The item a copy made by data-each shows (for e or the copy it is in); NULL when it is none. */
CscdModel* cscdViewModelOf(CscdView* v, CscdElement* e);

#ifdef __cplusplus
}
#endif

#endif
