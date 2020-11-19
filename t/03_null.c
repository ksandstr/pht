
/* funny business that the API doesn't support: NULL values to pht_add() and
 * pht_del().
 *
 * to compare, CCAN htable pops an assert when the pointer value to
 * htable_add() is NULL. for pht we'd like this to be testable.
 */

#include <stdbool.h>
#include <ccan/array_size/array_size.h>
#include <ccan/tap/tap.h>

#include "pht.h"


static size_t null_hash(const void *ptr, void *priv) {
	return 0;
}


int main(void)
{
	plan_tests(4);

	struct pht ht = PHT_INITIALIZER(ht, &null_hash, NULL);

	/* try to add and delete NULLs from the table. this won't work because
	 * iterators return "item-or-not" by NULL return value or not.
	 */
	bool ok = pht_add(&ht, null_hash(NULL, NULL), NULL);
	ok(!ok, "can't add NULL");
	ok1(pht_count(&ht) == 0);

	ok = pht_del(&ht, null_hash(NULL, NULL), NULL);
	ok(!ok, "can't del NULL");
	ok1(pht_count(&ht) == 0);

	return exit_status();
}
