#include "clar_libgit2.h"

#include <git2.h>

#include "multipack.h"

void test_pack_multipack__parse(void)
{
	git_repository *repo;
	struct git_multipack_index_file *idx;
	struct git_multipack_entry e;
	git_oid id;
	char midx_path[1025];

	cl_git_pass(git_repository_open(&repo, cl_fixture("testrepo.git")));
	p_snprintf(
			midx_path, sizeof(midx_path), "%s/objects/pack/multi-pack-index", git_repository_path(repo));
	cl_git_pass(git_multipack_index_open(&idx, midx_path));

	cl_git_pass(git_oid_fromstr(&id, "5001298e0c09ad9c34e4249bc5801c75e9754fa5"));
	cl_git_pass(git_multipack_index_entry_find(&e, idx, &id, GIT_OID_HEXSZ));
	cl_assert_equal_oid(&e.sha1, &id);
	cl_assert_equal_s(
			(const char *)git_vector_get(&idx->packfile_names, e.pack_index),
			"pack-d7c6adf9f61318f041845b01440d09aa7a91e1b5.idx");

	git_multipack_index_free(idx);
	git_repository_free(repo);
}

void test_pack_multipack__lookup(void)
{
	git_repository *repo;
	git_commit *commit;
	git_oid id;

	cl_git_pass(git_repository_open(&repo, cl_fixture("testrepo.git")));

	cl_git_pass(git_oid_fromstr(&id, "5001298e0c09ad9c34e4249bc5801c75e9754fa5"));
	cl_git_pass(git_commit_lookup_prefix(&commit, repo, &id, GIT_OID_HEXSZ));
	cl_assert_equal_s(git_commit_message(commit), "packed commit one\n");

	git_commit_free(commit);
	git_repository_free(repo);
}
