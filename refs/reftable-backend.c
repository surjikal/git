#include "../cache.h"
#include "../config.h"
#include "../refs.h"
#include "refs-internal.h"
#include "../iterator.h"
#include "../lockfile.h"
#include "../chdir-notify.h"

#include "../reftable/reftable.h"

extern struct ref_storage_be refs_be_reftable;

struct git_reftable_ref_store {
	struct ref_store base;
	unsigned int store_flags;

	int err;
	char *repo_dir;
	char *reftable_dir;
	struct reftable_stack *stack;
};

static int reftable_read_raw_ref(struct ref_store *ref_store,
				 const char *refname, struct object_id *oid,
				 struct strbuf *referent, unsigned int *type);

static void clear_reftable_log_record(struct reftable_log_record *log)
{
	log->old_hash = NULL;
	log->new_hash = NULL;
	log->message = NULL;
	log->ref_name = NULL;
	reftable_log_record_clear(log);
}

static void fill_reftable_log_record(struct reftable_log_record *log)
{
	const char *info = git_committer_info(0);
	struct ident_split split = { NULL };
	int result = split_ident_line(&split, info, strlen(info));
	int sign = 1;
	assert(0 == result);

	reftable_log_record_clear(log);
	log->name =
		xstrndup(split.name_begin, split.name_end - split.name_begin);
	log->email =
		xstrndup(split.mail_begin, split.mail_end - split.mail_begin);
	log->time = atol(split.date_begin);
	if (*split.tz_begin == '-') {
		sign = -1;
		split.tz_begin++;
	}
	if (*split.tz_begin == '+') {
		sign = 1;
		split.tz_begin++;
	}

	log->tz_offset = sign * atoi(split.tz_begin);
}

static struct ref_store *git_reftable_ref_store_create(const char *path,
						       unsigned int store_flags)
{
	struct git_reftable_ref_store *refs = xcalloc(1, sizeof(*refs));
	struct ref_store *ref_store = (struct ref_store *)refs;
	struct reftable_write_options cfg = {
		.block_size = 4096,
		.hash_id = the_hash_algo->format_id,
	};
	struct strbuf sb = STRBUF_INIT;

	base_ref_store_init(ref_store, &refs_be_reftable);
	refs->store_flags = store_flags;
	refs->repo_dir = xstrdup(path);
	strbuf_addf(&sb, "%s/reftable", path);
	refs->reftable_dir = xstrdup(sb.buf);
	strbuf_reset(&sb);

	refs->err = reftable_new_stack(&refs->stack, refs->reftable_dir, cfg);
	assert(refs->err != REFTABLE_API_ERROR);
	strbuf_release(&sb);
	return ref_store;
}

static int reftable_init_db(struct ref_store *ref_store, struct strbuf *err)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct strbuf sb = STRBUF_INIT;

	safe_create_dir(refs->reftable_dir, 1);

	strbuf_addf(&sb, "%s/HEAD", refs->repo_dir);
	write_file(sb.buf, "ref: refs/heads/.invalid");
	strbuf_reset(&sb);

	strbuf_addf(&sb, "%s/refs", refs->repo_dir);
	safe_create_dir(sb.buf, 1);
	strbuf_reset(&sb);

	strbuf_addf(&sb, "%s/refs/heads", refs->repo_dir);
	write_file(sb.buf, "this repository uses the reftable format");

	return 0;
}

struct git_reftable_iterator {
	struct ref_iterator base;
	struct reftable_iterator iter;
	struct reftable_ref_record ref;
	struct object_id oid;
	struct ref_store *ref_store;
	unsigned int flags;
	int err;
	const char *prefix;
};

static int reftable_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	while (ri->err == 0) {
		ri->err = reftable_iterator_next_ref(&ri->iter, &ri->ref);
		if (ri->err) {
			break;
		}

		/*
		   We could filter pseudo refs here explicitly, but HEAD is not
		   a PSEUDOREF, but a PER_WORKTREE, b/c each worktree can have
		   its own HEAD.
		 */
		ri->base.refname = ri->ref.ref_name;
		if (ri->prefix != NULL &&
		    strncmp(ri->prefix, ri->ref.ref_name, strlen(ri->prefix))) {
			ri->err = 1;
			break;
		}
		if (ri->flags & DO_FOR_EACH_PER_WORKTREE_ONLY &&
		    ref_type(ri->base.refname) != REF_TYPE_PER_WORKTREE)
			continue;

		ri->base.flags = 0;
		if (ri->ref.value != NULL) {
			hashcpy(ri->oid.hash, ri->ref.value);
		} else if (ri->ref.target != NULL) {
			int out_flags = 0;
			const char *resolved = refs_resolve_ref_unsafe(
				ri->ref_store, ri->ref.ref_name,
				RESOLVE_REF_READING, &ri->oid, &out_flags);
			ri->base.flags = out_flags;
			if (resolved == NULL &&
			    !(ri->flags & DO_FOR_EACH_INCLUDE_BROKEN) &&
			    (ri->base.flags & REF_ISBROKEN)) {
				continue;
			}
		}

		ri->base.oid = &ri->oid;
		if (!(ri->flags & DO_FOR_EACH_INCLUDE_BROKEN) &&
		    !ref_resolves_to_object(ri->base.refname, ri->base.oid,
					    ri->base.flags)) {
			continue;
		}

		break;
	}

	if (ri->err > 0) {
		return ITER_DONE;
	}
	if (ri->err < 0) {
		return ITER_ERROR;
	}

	return ITER_OK;
}

static int reftable_ref_iterator_peel(struct ref_iterator *ref_iterator,
				      struct object_id *peeled)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	if (ri->ref.target_value != NULL) {
		hashcpy(peeled->hash, ri->ref.target_value);
		return 0;
	}

	return -1;
}

static int reftable_ref_iterator_abort(struct ref_iterator *ref_iterator)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	reftable_ref_record_clear(&ri->ref);
	reftable_iterator_destroy(&ri->iter);
	return 0;
}

static struct ref_iterator_vtable reftable_ref_iterator_vtable = {
	reftable_ref_iterator_advance, reftable_ref_iterator_peel,
	reftable_ref_iterator_abort
};

static struct ref_iterator *
reftable_ref_iterator_begin(struct ref_store *ref_store, const char *prefix,
			    unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct git_reftable_iterator *ri = xcalloc(1, sizeof(*ri));
	struct reftable_merged_table *mt = NULL;

	if (refs->err < 0) {
		ri->err = refs->err;
	} else {
		mt = reftable_stack_merged_table(refs->stack);
		ri->err = reftable_merged_table_seek_ref(mt, &ri->iter, prefix);
	}

	base_ref_iterator_init(&ri->base, &reftable_ref_iterator_vtable, 1);
	ri->prefix = prefix;
	ri->base.oid = &ri->oid;
	ri->flags = flags;
	ri->ref_store = ref_store;
	return &ri->base;
}

static int fixup_symrefs(struct ref_store *ref_store,
			 struct ref_transaction *transaction)
{
	struct strbuf referent = STRBUF_INIT;
	int i = 0;
	int err = 0;

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *update = transaction->updates[i];
		struct object_id old_oid;

		err = reftable_read_raw_ref(ref_store, update->refname,
					    &old_oid, &referent,
					    /* mutate input, like
					       files-backend.c */
					    &update->type);
		if (err < 0 && errno == ENOENT &&
		    is_null_oid(&update->old_oid)) {
			err = 0;
		}
		if (err < 0)
			goto done;

		if (!(update->type & REF_ISSYMREF))
			continue;

		if (update->flags & REF_NO_DEREF) {
			/* what should happen here? See files-backend.c
			 * lock_ref_for_update. */
		} else {
			/*
			  If we are updating a symref (eg. HEAD), we should also
			  update the branch that the symref points to.

			  This is generic functionality, and would be better
			  done in refs.c, but the current implementation is
			  intertwined with the locking in files-backend.c.
			*/
			int new_flags = update->flags;
			struct ref_update *new_update = NULL;

			/* if this is an update for HEAD, should also record a
			   log entry for HEAD? See files-backend.c,
			   split_head_update()
			*/
			new_update = ref_transaction_add_update(
				transaction, referent.buf, new_flags,
				&update->new_oid, &update->old_oid,
				update->msg);
			new_update->parent_update = update;

			/* files-backend sets REF_LOG_ONLY here. */
			update->flags |= REF_NO_DEREF | REF_LOG_ONLY;
			update->flags &= ~REF_HAVE_OLD;
		}
	}

done:
	assert(err != REFTABLE_API_ERROR);
	strbuf_release(&referent);
	return err;
}

static int reftable_transaction_prepare(struct ref_store *ref_store,
					struct ref_transaction *transaction,
					struct strbuf *errbuf)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_addition *add = NULL;
	int err = refs->err;
	if (err < 0) {
		goto done;
	}

	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_new_addition(&add, refs->stack);
	if (err) {
		goto done;
	}

	err = fixup_symrefs(ref_store, transaction);
	if (err) {
		goto done;
	}

	transaction->backend_data = add;
	transaction->state = REF_TRANSACTION_PREPARED;

done:
	assert(err != REFTABLE_API_ERROR);
	if (err < 0) {
		transaction->state = REF_TRANSACTION_CLOSED;
		strbuf_addf(errbuf, "reftable: transaction prepare: %s",
			    reftable_error_str(err));
	}

	return err;
}

static int reftable_transaction_abort(struct ref_store *ref_store,
				      struct ref_transaction *transaction,
				      struct strbuf *err)
{
	struct reftable_addition *add =
		(struct reftable_addition *)transaction->backend_data;
	reftable_addition_destroy(add);
	transaction->backend_data = NULL;
	return 0;
}

static int reftable_check_old_oid(struct ref_store *refs, const char *refname,
				  struct object_id *want_oid)
{
	struct object_id out_oid;
	int out_flags = 0;
	const char *resolved = refs_resolve_ref_unsafe(
		refs, refname, RESOLVE_REF_READING, &out_oid, &out_flags);
	if (is_null_oid(want_oid) != (resolved == NULL)) {
		return REFTABLE_LOCK_ERROR;
	}

	if (resolved != NULL && !oideq(&out_oid, want_oid)) {
		return REFTABLE_LOCK_ERROR;
	}

	return 0;
}

static int ref_update_cmp(const void *a, const void *b)
{
	return strcmp((*(struct ref_update **)a)->refname,
		      (*(struct ref_update **)b)->refname);
}

static int write_transaction_table(struct reftable_writer *writer, void *arg)
{
	struct ref_transaction *transaction = (struct ref_transaction *)arg;
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)transaction->ref_store;
	uint64_t ts = reftable_stack_next_update_index(refs->stack);
	int err = 0;
	int i = 0;
	struct reftable_log_record *logs =
		calloc(transaction->nr, sizeof(*logs));
	struct ref_update **sorted =
		malloc(transaction->nr * sizeof(struct ref_update *));
	COPY_ARRAY(sorted, transaction->updates, transaction->nr);
	QSORT(sorted, transaction->nr, ref_update_cmp);
	reftable_writer_set_limits(writer, ts, ts);

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *u = sorted[i];
		struct reftable_log_record *log = &logs[i];
		fill_reftable_log_record(log);
		log->ref_name = (char *)u->refname;
		log->old_hash = u->old_oid.hash;
		log->new_hash = u->new_oid.hash;
		log->update_index = ts;
		log->message = u->msg;

		if (u->flags & REF_LOG_ONLY) {
			continue;
		}

		if (u->flags & REF_HAVE_NEW) {
			struct reftable_ref_record ref = { NULL };
			struct object_id peeled;

			int peel_error = peel_object(&u->new_oid, &peeled);
			ref.ref_name = (char *)u->refname;

			if (!is_null_oid(&u->new_oid)) {
				ref.value = u->new_oid.hash;
			}
			ref.update_index = ts;
			if (!peel_error) {
				ref.target_value = peeled.hash;
			}

			err = reftable_writer_add_ref(writer, &ref);
			if (err < 0) {
				goto done;
			}
		}
	}

	for (i = 0; i < transaction->nr; i++) {
		err = reftable_writer_add_log(writer, &logs[i]);
		clear_reftable_log_record(&logs[i]);
		if (err < 0) {
			goto done;
		}
	}

done:
	assert(err != REFTABLE_API_ERROR);
	free(logs);
	free(sorted);
	return err;
}

static int reftable_transaction_finish(struct ref_store *ref_store,
				       struct ref_transaction *transaction,
				       struct strbuf *errmsg)
{
	struct reftable_addition *add =
		(struct reftable_addition *)transaction->backend_data;
	int err = 0;
	int i;

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *u = transaction->updates[i];
		if (u->flags & REF_HAVE_OLD) {
			err = reftable_check_old_oid(transaction->ref_store,
						     u->refname, &u->old_oid);
			if (err < 0) {
				goto done;
			}
		}
	}

	err = reftable_addition_add(add, &write_transaction_table, transaction);
	if (err < 0) {
		goto done;
	}

	err = reftable_addition_commit(add);

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_addition_destroy(add);
	transaction->state = REF_TRANSACTION_CLOSED;
	transaction->backend_data = NULL;
	if (err) {
		strbuf_addf(errmsg, "reftable: transaction failure: %s",
			    reftable_error_str(err));
		return -1;
	}
	return err;
}

static int
reftable_transaction_initial_commit(struct ref_store *ref_store,
				    struct ref_transaction *transaction,
				    struct strbuf *errmsg)
{
	int err = reftable_transaction_prepare(ref_store, transaction, errmsg);
	if (err)
		return err;

	return reftable_transaction_finish(ref_store, transaction, errmsg);
}

struct write_pseudoref_arg {
	struct reftable_stack *stack;
	const char *pseudoref;
	const struct object_id *new_oid;
	const struct object_id *old_oid;
};

static int write_pseudoref_table(struct reftable_writer *writer, void *argv)
{
	struct write_pseudoref_arg *arg = (struct write_pseudoref_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	int err = 0;
	struct reftable_ref_record read_ref = { NULL };
	struct reftable_ref_record write_ref = { NULL };

	reftable_writer_set_limits(writer, ts, ts);
	if (arg->old_oid) {
		struct object_id read_oid;
		err = reftable_stack_read_ref(arg->stack, arg->pseudoref,
					      &read_ref);
		if (err < 0)
			goto done;

		if ((err > 0) != is_null_oid(arg->old_oid)) {
			err = REFTABLE_LOCK_ERROR;
			goto done;
		}

		/* XXX If old_oid is set, and we have a symref? */

		if (err == 0 && read_ref.value == NULL) {
			err = REFTABLE_LOCK_ERROR;
			goto done;
		}

		hashcpy(read_oid.hash, read_ref.value);
		if (!oideq(arg->old_oid, &read_oid)) {
			err = REFTABLE_LOCK_ERROR;
			goto done;
		}
	}

	write_ref.ref_name = (char *)arg->pseudoref;
	write_ref.update_index = ts;
	if (!is_null_oid(arg->new_oid))
		write_ref.value = (uint8_t *)arg->new_oid->hash;

	err = reftable_writer_add_ref(writer, &write_ref);
done:
	assert(err != REFTABLE_API_ERROR);
	reftable_ref_record_clear(&read_ref);
	return err;
}

static int reftable_write_pseudoref(struct ref_store *ref_store,
				    const char *pseudoref,
				    const struct object_id *oid,
				    const struct object_id *old_oid,
				    struct strbuf *errbuf)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct write_pseudoref_arg arg = {
		.stack = refs->stack,
		.pseudoref = pseudoref,
		.new_oid = oid,
	};
	struct reftable_addition *add = NULL;
	int err = refs->err;
	if (err < 0) {
		goto done;
	}

	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}
	err = reftable_stack_new_addition(&add, refs->stack);
	if (err) {
		goto done;
	}
	if (old_oid) {
		struct object_id actual_old_oid;

		/* XXX this is cut & paste from files-backend - should factor
		 * out? */
		if (read_ref(pseudoref, &actual_old_oid)) {
			if (!is_null_oid(old_oid)) {
				strbuf_addf(errbuf,
					    _("could not read ref '%s'"),
					    pseudoref);
				goto done;
			}
		} else if (is_null_oid(old_oid)) {
			strbuf_addf(errbuf, _("ref '%s' already exists"),
				    pseudoref);
			goto done;
		} else if (!oideq(&actual_old_oid, old_oid)) {
			strbuf_addf(errbuf,
				    _("unexpected object ID when writing '%s'"),
				    pseudoref);
			goto done;
		}
	}

	err = reftable_addition_add(add, &write_pseudoref_table, &arg);
	if (err < 0) {
		strbuf_addf(errbuf, "reftable: pseudoref update failure: %s",
			    reftable_error_str(err));
	}

	err = reftable_addition_commit(add);
	if (err < 0) {
		strbuf_addf(errbuf, "reftable: pseudoref commit failure: %s",
			    reftable_error_str(err));
	}

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_addition_destroy(add);
	return err;
}

static int reftable_delete_pseudoref(struct ref_store *ref_store,
				     const char *pseudoref,
				     const struct object_id *old_oid)
{
	struct strbuf errbuf = STRBUF_INIT;
	int ret = reftable_write_pseudoref(ref_store, pseudoref, &null_oid,
					   old_oid, &errbuf);
	/* XXX what to do with the error message? */
	strbuf_release(&errbuf);
	return ret;
}

struct write_delete_refs_arg {
	struct reftable_stack *stack;
	struct string_list *refnames;
	const char *logmsg;
	unsigned int flags;
};

static int write_delete_refs_table(struct reftable_writer *writer, void *argv)
{
	struct write_delete_refs_arg *arg =
		(struct write_delete_refs_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	int err = 0;
	int i = 0;

	reftable_writer_set_limits(writer, ts, ts);
	for (i = 0; i < arg->refnames->nr; i++) {
		struct reftable_ref_record ref = {
			.ref_name = (char *)arg->refnames->items[i].string,
			.update_index = ts,
		};
		err = reftable_writer_add_ref(writer, &ref);
		if (err < 0) {
			return err;
		}
	}

	for (i = 0; i < arg->refnames->nr; i++) {
		struct reftable_log_record log = {
			.update_index = ts,
		};
		struct reftable_ref_record current = { NULL };
		fill_reftable_log_record(&log);
		log.message = xstrdup(arg->logmsg);
		log.new_hash = NULL;
		log.old_hash = NULL;
		log.update_index = ts;
		log.ref_name = (char *)arg->refnames->items[i].string;

		if (reftable_stack_read_ref(arg->stack, log.ref_name,
					    &current) == 0) {
			log.old_hash = current.value;
		}
		err = reftable_writer_add_log(writer, &log);
		log.old_hash = NULL;
		reftable_ref_record_clear(&current);

		clear_reftable_log_record(&log);
		if (err < 0) {
			return err;
		}
	}
	return 0;
}

static int reftable_delete_refs(struct ref_store *ref_store, const char *msg,
				struct string_list *refnames,
				unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct write_delete_refs_arg arg = {
		.stack = refs->stack,
		.refnames = refnames,
		.logmsg = msg,
		.flags = flags,
	};
	int err = refs->err;
	if (err < 0) {
		goto done;
	}

	string_list_sort(refnames);
	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}
	err = reftable_stack_add(refs->stack, &write_delete_refs_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

static int reftable_pack_refs(struct ref_store *ref_store, unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	if (refs->err < 0) {
		return refs->err;
	}
	return reftable_stack_compact_all(refs->stack, NULL);
}

struct write_create_symref_arg {
	struct git_reftable_ref_store *refs;
	const char *refname;
	const char *target;
	const char *logmsg;
};

static int write_create_symref_table(struct reftable_writer *writer, void *arg)
{
	struct write_create_symref_arg *create =
		(struct write_create_symref_arg *)arg;
	uint64_t ts = reftable_stack_next_update_index(create->refs->stack);
	int err = 0;

	struct reftable_ref_record ref = {
		.ref_name = (char *)create->refname,
		.target = (char *)create->target,
		.update_index = ts,
	};
	reftable_writer_set_limits(writer, ts, ts);
	err = reftable_writer_add_ref(writer, &ref);
	if (err == 0) {
		struct reftable_log_record log = { NULL };
		struct object_id new_oid;
		struct object_id old_oid;

		fill_reftable_log_record(&log);
		log.ref_name = (char *)create->refname;
		log.message = (char *)create->logmsg;
		log.update_index = ts;
		if (refs_resolve_ref_unsafe(
			    (struct ref_store *)create->refs, create->refname,
			    RESOLVE_REF_READING, &old_oid, NULL) != NULL) {
			log.old_hash = old_oid.hash;
		}

		if (refs_resolve_ref_unsafe((struct ref_store *)create->refs,
					    create->target, RESOLVE_REF_READING,
					    &new_oid, NULL) != NULL) {
			log.new_hash = new_oid.hash;
		}

		if (log.old_hash != NULL || log.new_hash != NULL) {
			err = reftable_writer_add_log(writer, &log);
		}
		log.ref_name = NULL;
		log.message = NULL;
		log.old_hash = NULL;
		log.new_hash = NULL;
		clear_reftable_log_record(&log);
	}
	return err;
}

static int reftable_create_symref(struct ref_store *ref_store,
				  const char *refname, const char *target,
				  const char *logmsg)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct write_create_symref_arg arg = { .refs = refs,
					       .refname = refname,
					       .target = target,
					       .logmsg = logmsg };
	int err = refs->err;
	if (err < 0) {
		goto done;
	}
	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}
	err = reftable_stack_add(refs->stack, &write_create_symref_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

struct write_rename_arg {
	struct reftable_stack *stack;
	const char *oldname;
	const char *newname;
	const char *logmsg;
};

static int write_rename_table(struct reftable_writer *writer, void *argv)
{
	struct write_rename_arg *arg = (struct write_rename_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	struct reftable_ref_record ref = { NULL };
	int err = reftable_stack_read_ref(arg->stack, arg->oldname, &ref);

	if (err) {
		goto done;
	}

	/* XXX do ref renames overwrite the target? */
	if (reftable_stack_read_ref(arg->stack, arg->newname, &ref) == 0) {
		goto done;
	}

	free(ref.ref_name);
	ref.ref_name = strdup(arg->newname);
	reftable_writer_set_limits(writer, ts, ts);
	ref.update_index = ts;

	{
		struct reftable_ref_record todo[2] = { { NULL } };
		todo[0].ref_name = (char *)arg->oldname;
		todo[0].update_index = ts;
		/* leave todo[0] empty */
		todo[1] = ref;
		todo[1].update_index = ts;

		err = reftable_writer_add_refs(writer, todo, 2);
		if (err < 0) {
			goto done;
		}
	}

	if (ref.value != NULL) {
		struct reftable_log_record todo[2] = { { NULL } };
		fill_reftable_log_record(&todo[0]);
		fill_reftable_log_record(&todo[1]);

		todo[0].ref_name = (char *)arg->oldname;
		todo[0].update_index = ts;
		todo[0].message = (char *)arg->logmsg;
		todo[0].old_hash = ref.value;
		todo[0].new_hash = NULL;

		todo[1].ref_name = (char *)arg->newname;
		todo[1].update_index = ts;
		todo[1].old_hash = NULL;
		todo[1].new_hash = ref.value;
		todo[1].message = (char *)arg->logmsg;

		err = reftable_writer_add_logs(writer, todo, 2);

		clear_reftable_log_record(&todo[0]);
		clear_reftable_log_record(&todo[1]);

		if (err < 0) {
			goto done;
		}

	} else {
		/* XXX symrefs? */
	}

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_ref_record_clear(&ref);
	return err;
}

static int reftable_rename_ref(struct ref_store *ref_store,
			       const char *oldrefname, const char *newrefname,
			       const char *logmsg)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct write_rename_arg arg = {
		.stack = refs->stack,
		.oldname = oldrefname,
		.newname = newrefname,
		.logmsg = logmsg,
	};
	int err = refs->err;
	if (err < 0) {
		goto done;
	}
	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_add(refs->stack, &write_rename_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

static int reftable_copy_ref(struct ref_store *ref_store,
			     const char *oldrefname, const char *newrefname,
			     const char *logmsg)
{
	BUG("reftable reference store does not support copying references");
}

struct reftable_reflog_ref_iterator {
	struct ref_iterator base;
	struct reftable_iterator iter;
	struct reftable_log_record log;
	struct object_id oid;
	char *last_name;
};

static int
reftable_reflog_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct reftable_reflog_ref_iterator *ri =
		(struct reftable_reflog_ref_iterator *)ref_iterator;

	while (1) {
		int err = reftable_iterator_next_log(&ri->iter, &ri->log);
		if (err > 0) {
			return ITER_DONE;
		}
		if (err < 0) {
			return ITER_ERROR;
		}

		ri->base.refname = ri->log.ref_name;
		if (ri->last_name != NULL &&
		    !strcmp(ri->log.ref_name, ri->last_name)) {
			/* we want the refnames that we have reflogs for, so we
			 * skip if we've already produced this name. This could
			 * be faster by seeking directly to
			 * reflog@update_index==0.
			 */
			continue;
		}

		free(ri->last_name);
		ri->last_name = xstrdup(ri->log.ref_name);
		hashcpy(ri->oid.hash, ri->log.new_hash);
		return ITER_OK;
	}
}

static int reftable_reflog_ref_iterator_peel(struct ref_iterator *ref_iterator,
					     struct object_id *peeled)
{
	BUG("not supported.");
	return -1;
}

static int reftable_reflog_ref_iterator_abort(struct ref_iterator *ref_iterator)
{
	struct reftable_reflog_ref_iterator *ri =
		(struct reftable_reflog_ref_iterator *)ref_iterator;
	reftable_log_record_clear(&ri->log);
	reftable_iterator_destroy(&ri->iter);
	return 0;
}

static struct ref_iterator_vtable reftable_reflog_ref_iterator_vtable = {
	reftable_reflog_ref_iterator_advance, reftable_reflog_ref_iterator_peel,
	reftable_reflog_ref_iterator_abort
};

static struct ref_iterator *
reftable_reflog_iterator_begin(struct ref_store *ref_store)
{
	struct reftable_reflog_ref_iterator *ri = xcalloc(sizeof(*ri), 1);
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;

	struct reftable_merged_table *mt =
		reftable_stack_merged_table(refs->stack);
	int err = reftable_merged_table_seek_log(mt, &ri->iter, "");
	if (err < 0) {
		free(ri);
		return NULL;
	}

	base_ref_iterator_init(&ri->base, &reftable_reflog_ref_iterator_vtable,
			       1);
	ri->base.oid = &ri->oid;

	return (struct ref_iterator *)ri;
}

static int
reftable_for_each_reflog_ent_newest_first(struct ref_store *ref_store,
					  const char *refname,
					  each_reflog_ent_fn fn, void *cb_data)
{
	struct reftable_iterator it = { NULL };
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_merged_table *mt = NULL;
	int err = 0;
	struct reftable_log_record log = { NULL };

	if (refs->err < 0) {
		return refs->err;
	}

	mt = reftable_stack_merged_table(refs->stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);
	while (err == 0) {
		struct object_id old_oid;
		struct object_id new_oid;
		const char *full_committer = "";

		err = reftable_iterator_next_log(&it, &log);
		if (err > 0) {
			err = 0;
			break;
		}
		if (err < 0) {
			break;
		}

		if (strcmp(log.ref_name, refname)) {
			break;
		}

		hashcpy(old_oid.hash, log.old_hash);
		hashcpy(new_oid.hash, log.new_hash);

		full_committer = fmt_ident(log.name, log.email,
					   WANT_COMMITTER_IDENT,
					   /*date*/ NULL, IDENT_NO_DATE);
		err = fn(&old_oid, &new_oid, full_committer, log.time,
			 log.tz_offset, log.message, cb_data);
		if (err)
			break;
	}

	reftable_log_record_clear(&log);
	reftable_iterator_destroy(&it);
	return err;
}

static int
reftable_for_each_reflog_ent_oldest_first(struct ref_store *ref_store,
					  const char *refname,
					  each_reflog_ent_fn fn, void *cb_data)
{
	struct reftable_iterator it = { NULL };
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_merged_table *mt = NULL;
	struct reftable_log_record *logs = NULL;
	int cap = 0;
	int len = 0;
	int err = 0;
	int i = 0;

	if (refs->err < 0) {
		return refs->err;
	}
	mt = reftable_stack_merged_table(refs->stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);

	while (err == 0) {
		struct reftable_log_record log = { NULL };
		err = reftable_iterator_next_log(&it, &log);
		if (err > 0) {
			err = 0;
			break;
		}
		if (err < 0) {
			break;
		}

		if (strcmp(log.ref_name, refname)) {
			break;
		}

		if (len == cap) {
			cap = 2 * cap + 1;
			logs = realloc(logs, cap * sizeof(*logs));
		}

		logs[len++] = log;
	}

	for (i = len; i--;) {
		struct reftable_log_record *log = &logs[i];
		struct object_id old_oid;
		struct object_id new_oid;
		const char *full_committer = "";

		hashcpy(old_oid.hash, log->old_hash);
		hashcpy(new_oid.hash, log->new_hash);

		full_committer = fmt_ident(log->name, log->email,
					   WANT_COMMITTER_IDENT, NULL,
					   IDENT_NO_DATE);
		err = fn(&old_oid, &new_oid, full_committer, log->time,
			 log->tz_offset, log->message, cb_data);
		if (err) {
			break;
		}
	}

	for (i = 0; i < len; i++) {
		reftable_log_record_clear(&logs[i]);
	}
	free(logs);

	reftable_iterator_destroy(&it);
	return err;
}

static int reftable_reflog_exists(struct ref_store *ref_store,
				  const char *refname)
{
	/* always exists. */
	return 1;
}

static int reftable_create_reflog(struct ref_store *ref_store,
				  const char *refname, int force_create,
				  struct strbuf *err)
{
	return 0;
}

static int reftable_delete_reflog(struct ref_store *ref_store,
				  const char *refname)
{
	return 0;
}

struct reflog_expiry_arg {
	struct git_reftable_ref_store *refs;
	struct reftable_log_record *tombstones;
	int len;
	int cap;
};

static void clear_log_tombstones(struct reflog_expiry_arg *arg)
{
	int i = 0;
	for (; i < arg->len; i++) {
		reftable_log_record_clear(&arg->tombstones[i]);
	}

	FREE_AND_NULL(arg->tombstones);
}

static void add_log_tombstone(struct reflog_expiry_arg *arg,
			      const char *refname, uint64_t ts)
{
	struct reftable_log_record tombstone = {
		.ref_name = xstrdup(refname),
		.update_index = ts,
	};
	if (arg->len == arg->cap) {
		arg->cap = 2 * arg->cap + 1;
		arg->tombstones =
			realloc(arg->tombstones, arg->cap * sizeof(tombstone));
	}
	arg->tombstones[arg->len++] = tombstone;
}

static int write_reflog_expiry_table(struct reftable_writer *writer, void *argv)
{
	struct reflog_expiry_arg *arg = (struct reflog_expiry_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->refs->stack);
	int i = 0;
	reftable_writer_set_limits(writer, ts, ts);
	for (i = 0; i < arg->len; i++) {
		int err = reftable_writer_add_log(writer, &arg->tombstones[i]);
		if (err) {
			return err;
		}
	}
	return 0;
}

static int reftable_reflog_expire(struct ref_store *ref_store,
				  const char *refname,
				  const struct object_id *oid,
				  unsigned int flags,
				  reflog_expiry_prepare_fn prepare_fn,
				  reflog_expiry_should_prune_fn should_prune_fn,
				  reflog_expiry_cleanup_fn cleanup_fn,
				  void *policy_cb_data)
{
	/*
	  For log expiry, we write tombstones in place of the expired entries,
	  This means that the entries are still retrievable by delving into the
	  stack, and expiring entries paradoxically takes extra memory.

	  This memory is only reclaimed when some operation issues a
	  reftable_pack_refs(), which will compact the entire stack and get rid
	  of deletion entries.

	  It would be better if the refs backend supported an API that sets a
	  criterion for all refs, passing the criterion to pack_refs().
	*/
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_merged_table *mt = NULL;
	struct reflog_expiry_arg arg = {
		.refs = refs,
	};
	struct reftable_log_record log = { NULL };
	struct reftable_iterator it = { NULL };
	int err = 0;
	if (refs->err < 0) {
		return refs->err;
	}
	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}

	mt = reftable_stack_merged_table(refs->stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);
	if (err < 0) {
		goto done;
	}

	while (1) {
		struct object_id ooid;
		struct object_id noid;

		int err = reftable_iterator_next_log(&it, &log);
		if (err < 0) {
			goto done;
		}

		if (err > 0 || strcmp(log.ref_name, refname)) {
			break;
		}
		hashcpy(ooid.hash, log.old_hash);
		hashcpy(noid.hash, log.new_hash);

		if (should_prune_fn(&ooid, &noid, log.email,
				    (timestamp_t)log.time, log.tz_offset,
				    log.message, policy_cb_data)) {
			add_log_tombstone(&arg, refname, log.update_index);
		}
	}
	err = reftable_stack_add(refs->stack, &write_reflog_expiry_table, &arg);

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_log_record_clear(&log);
	reftable_iterator_destroy(&it);
	clear_log_tombstones(&arg);
	return err;
}

static int reftable_read_raw_ref(struct ref_store *ref_store,
				 const char *refname, struct object_id *oid,
				 struct strbuf *referent, unsigned int *type)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_ref_record ref = { NULL };
	int err = 0;
	if (refs->err < 0) {
		return refs->err;
	}

	/* This is usually not needed, but Git doesn't signal to ref backend if
	   a subprocess updated the ref DB.  So we always check.
	*/
	err = reftable_stack_reload(refs->stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_read_ref(refs->stack, refname, &ref);
	if (err > 0) {
		errno = ENOENT;
		err = -1;
		goto done;
	}
	if (err < 0) {
		errno = reftable_error_to_errno(err);
		err = -1;
		goto done;
	}
	if (ref.target != NULL) {
		strbuf_reset(referent);
		strbuf_addstr(referent, ref.target);
		*type |= REF_ISSYMREF;
	} else if (ref.value != NULL) {
		hashcpy(oid->hash, ref.value);
	} else {
		*type |= REF_ISBROKEN;
		errno = EINVAL;
		err = -1;
	}
done:
	assert(err != REFTABLE_API_ERROR);
	reftable_ref_record_clear(&ref);
	return err;
}

struct ref_storage_be refs_be_reftable = {
	&refs_be_files,
	"reftable",
	git_reftable_ref_store_create,
	reftable_init_db,
	reftable_transaction_prepare,
	reftable_transaction_finish,
	reftable_transaction_abort,
	reftable_transaction_initial_commit,

	reftable_pack_refs,
	reftable_create_symref,
	reftable_delete_refs,
	reftable_rename_ref,
	reftable_copy_ref,

	reftable_write_pseudoref,
	reftable_delete_pseudoref,

	reftable_ref_iterator_begin,
	reftable_read_raw_ref,

	reftable_reflog_iterator_begin,
	reftable_for_each_reflog_ent_oldest_first,
	reftable_for_each_reflog_ent_newest_first,
	reftable_reflog_exists,
	reftable_create_reflog,
	reftable_delete_reflog,
	reftable_reflog_expire,
};
