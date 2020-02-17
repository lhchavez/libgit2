#include "clar_libgit2.h"

#include <git2.h>
#include <git2/sys/multipack.h>

#include "multipack.h"

void test_pack_multipack__parse(void)
{
	git_repository *repo;
	struct git_multipack_index_file *idx;
	struct git_multipack_entry e;
	git_oid id;
	git_buf midx_path = GIT_BUF_INIT;

	cl_git_pass(git_repository_open(&repo, cl_fixture("testrepo.git")));
	cl_git_pass(git_buf_joinpath(&midx_path, git_repository_path(repo), "objects/pack/multi-pack-index"));
	cl_git_pass(git_multipack_index_open(&idx, git_buf_cstr(&midx_path)));

	cl_git_pass(git_oid_fromstr(&id, "5001298e0c09ad9c34e4249bc5801c75e9754fa5"));
	cl_git_pass(git_multipack_index_entry_find(&e, idx, &id, GIT_OID_HEXSZ));
	cl_assert_equal_oid(&e.sha1, &id);
	cl_assert_equal_s(
			(const char *)git_vector_get(&idx->packfile_names, e.pack_index),
			"pack-d7c6adf9f61318f041845b01440d09aa7a91e1b5.idx");

	git_multipack_index_free(idx);
	git_repository_free(repo);
	git_buf_dispose(&midx_path);
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

void test_pack_multipack__writer(void)
{
	git_repository *repo;
	git_multipack_index_writer *w = NULL;
	git_buf midx = GIT_BUF_INIT, expected_midx = GIT_BUF_INIT, path = GIT_BUF_INIT;

	cl_git_pass(git_repository_open(&repo, cl_fixture("testrepo.git")));

	cl_git_pass(git_buf_joinpath(&path, git_repository_path(repo), "objects/pack"));
	cl_git_pass(git_multipack_index_writer_new(&w, git_buf_cstr(&path)));

	cl_git_pass(git_multipack_index_writer_add(w, "pack-d7c6adf9f61318f041845b01440d09aa7a91e1b5.idx"));
	cl_git_pass(git_multipack_index_writer_add(w, "pack-d85f5d483273108c9d8dd0e4728ccf0b2982423a.idx"));
	cl_git_pass(git_multipack_index_writer_add(w, "pack-a81e489679b7d3418f9ab594bda8ceb37dd4c695.idx"));

	cl_git_pass(git_multipack_index_writer_dump(&midx, w));
	cl_git_pass(git_buf_joinpath(&path, git_repository_path(repo), "objects/pack/multi-pack-index"));
	cl_git_pass(git_futils_readbuffer(&expected_midx, git_buf_cstr(&path)));

	cl_assert_equal_i(git_buf_len(&midx), git_buf_len(&expected_midx));
	cl_assert_equal_strn(git_buf_cstr(&midx), git_buf_cstr(&expected_midx), git_buf_len(&midx));

	git_buf_dispose(&midx);
	git_buf_dispose(&expected_midx);
	git_buf_dispose(&path);
	git_multipack_index_writer_free(w);
	git_repository_free(repo);
}
