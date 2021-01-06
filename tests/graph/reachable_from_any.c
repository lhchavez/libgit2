#include "clar_libgit2.h"

#include <git2.h>

#include "commit_graph.h"

static git_repository *repo;

#define TEST_REPO_PATH "merge-recursive"

void test_graph_reachable_from_any__initialize(void)
{
	git_oid oid;
	git_commit *commit;

	repo = cl_git_sandbox_init(TEST_REPO_PATH);

	git_oid_fromstr(&oid, "539bd011c4822c560c1d17cab095006b7a10f707");
	cl_git_pass(git_commit_lookup(&commit, repo, &oid));
	cl_git_pass(git_reset(repo, (git_object *)commit, GIT_RESET_HARD, NULL));
	git_commit_free(commit);
}

void test_graph_reachable_from_any__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

void test_graph_reachable_from_any__returns_correct_result(void)
{
	git_object *branchA1, *branchA2, *branchB1, *branchB2, *branchC1, *branchC2, *branchH1,
			*branchH2;
	git_oid descendants[7];

	cl_git_pass(git_revparse_single(&branchA1, repo, "branchA-1"));
	cl_git_pass(git_revparse_single(&branchA2, repo, "branchA-2"));
	cl_git_pass(git_revparse_single(&branchB1, repo, "branchB-1"));
	cl_git_pass(git_revparse_single(&branchB2, repo, "branchB-2"));
	cl_git_pass(git_revparse_single(&branchC1, repo, "branchC-1"));
	cl_git_pass(git_revparse_single(&branchC2, repo, "branchC-2"));
	cl_git_pass(git_revparse_single(&branchH1, repo, "branchH-1"));
	cl_git_pass(git_revparse_single(&branchH2, repo, "branchH-2"));

	cl_assert_equal_i(
			git_graph_reachable_from_any(
					repo, git_object_id(branchH1), 1, git_object_id(branchA1)),
			0);
	cl_assert_equal_i(
			git_graph_reachable_from_any(
					repo, git_object_id(branchH1), 1, git_object_id(branchA2)),
			0);

	cl_git_pass(git_oid_cpy(&descendants[0], git_object_id(branchA1)));
	cl_git_pass(git_oid_cpy(&descendants[1], git_object_id(branchA2)));
	cl_git_pass(git_oid_cpy(&descendants[2], git_object_id(branchB1)));
	cl_git_pass(git_oid_cpy(&descendants[3], git_object_id(branchB2)));
	cl_git_pass(git_oid_cpy(&descendants[4], git_object_id(branchC1)));
	cl_git_pass(git_oid_cpy(&descendants[5], git_object_id(branchC2)));
	cl_git_pass(git_oid_cpy(&descendants[6], git_object_id(branchH2)));
	cl_assert_equal_i(
			git_graph_reachable_from_any(repo, git_object_id(branchH2), 6, descendants),
			0);
	cl_assert_equal_i(
			git_graph_reachable_from_any(repo, git_object_id(branchH2), 7, descendants),
			1);

	git_object_free(branchA1);
	git_object_free(branchA2);
	git_object_free(branchB1);
	git_object_free(branchB2);
	git_object_free(branchC1);
	git_object_free(branchC2);
	git_object_free(branchH1);
	git_object_free(branchH2);
}
