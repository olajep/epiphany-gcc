/* Conversion of SESE regions to Polyhedra.
   Copyright (C) 2009, 2010, 2011 Free Software Foundation, Inc.
   Contributed by Sebastian Pop <sebastian.pop@amd.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree-flow.h"
#include "tree-dump.h"
#include "cfgloop.h"
#include "tree-chrec.h"
#include "tree-data-ref.h"
#include "tree-scalar-evolution.h"
#include "domwalk.h"
#include "sese.h"

#ifdef HAVE_cloog
#include "ppl_c.h"
#include "graphite-ppl.h"
#include "graphite-poly.h"
#include "graphite-sese-to-poly.h"

/* Returns the index of the PHI argument defined in the outermost
   loop.  */

static size_t
phi_arg_in_outermost_loop (gimple phi)
{
  loop_p loop = gimple_bb (phi)->loop_father;
  size_t i, res = 0;

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    if (!flow_bb_inside_loop_p (loop, gimple_phi_arg_edge (phi, i)->src))
      {
	loop = gimple_phi_arg_edge (phi, i)->src->loop_father;
	res = i;
      }

  return res;
}

/* Removes a simple copy phi node "RES = phi (INIT, RES)" at position
   PSI by inserting on the loop ENTRY edge assignment "RES = INIT".  */

static void
remove_simple_copy_phi (gimple_stmt_iterator *psi)
{
  gimple phi = gsi_stmt (*psi);
  tree res = gimple_phi_result (phi);
  size_t entry = phi_arg_in_outermost_loop (phi);
  tree init = gimple_phi_arg_def (phi, entry);
  gimple stmt = gimple_build_assign (res, init);
  edge e = gimple_phi_arg_edge (phi, entry);

  remove_phi_node (psi, false);
  gsi_insert_on_edge_immediate (e, stmt);
  SSA_NAME_DEF_STMT (res) = stmt;
}

/* Removes an invariant phi node at position PSI by inserting on the
   loop ENTRY edge the assignment RES = INIT.  */

static void
remove_invariant_phi (sese region, gimple_stmt_iterator *psi)
{
  gimple phi = gsi_stmt (*psi);
  loop_p loop = loop_containing_stmt (phi);
  tree res = gimple_phi_result (phi);
  tree scev = scalar_evolution_in_region (region, loop, res);
  size_t entry = phi_arg_in_outermost_loop (phi);
  edge e = gimple_phi_arg_edge (phi, entry);
  tree var;
  gimple stmt;
  gimple_seq stmts;
  gimple_stmt_iterator gsi;

  if (tree_contains_chrecs (scev, NULL))
    scev = gimple_phi_arg_def (phi, entry);

  var = force_gimple_operand (scev, &stmts, true, NULL_TREE);
  stmt = gimple_build_assign (res, var);
  remove_phi_node (psi, false);

  if (!stmts)
    stmts = gimple_seq_alloc ();

  gsi = gsi_last (stmts);
  gsi_insert_after (&gsi, stmt, GSI_NEW_STMT);
  gsi_insert_seq_on_edge (e, stmts);
  gsi_commit_edge_inserts ();
  SSA_NAME_DEF_STMT (res) = stmt;
}

/* Returns true when the phi node at PSI is of the form "a = phi (a, x)".  */

static inline bool
simple_copy_phi_p (gimple phi)
{
  tree res;

  if (gimple_phi_num_args (phi) != 2)
    return false;

  res = gimple_phi_result (phi);
  return (res == gimple_phi_arg_def (phi, 0)
	  || res == gimple_phi_arg_def (phi, 1));
}

/* Returns true when the phi node at position PSI is a reduction phi
   node in REGION.  Otherwise moves the pointer PSI to the next phi to
   be considered.  */

static bool
reduction_phi_p (sese region, gimple_stmt_iterator *psi)
{
  loop_p loop;
  gimple phi = gsi_stmt (*psi);
  tree res = gimple_phi_result (phi);

  loop = loop_containing_stmt (phi);

  if (simple_copy_phi_p (phi))
    {
      /* PRE introduces phi nodes like these, for an example,
	 see id-5.f in the fortran graphite testsuite:

	 # prephitmp.85_265 = PHI <prephitmp.85_258(33), prephitmp.85_265(18)>
      */
      remove_simple_copy_phi (psi);
      return false;
    }

  if (scev_analyzable_p (res, region))
    {
      tree scev = scalar_evolution_in_region (region, loop, res);

      if (evolution_function_is_invariant_p (scev, loop->num))
	remove_invariant_phi (region, psi);
      else
	gsi_next (psi);

      return false;
    }

  /* All the other cases are considered reductions.  */
  return true;
}

/* Store the GRAPHITE representation of BB.  */

static gimple_bb_p
new_gimple_bb (basic_block bb, VEC (data_reference_p, heap) *drs)
{
  struct gimple_bb *gbb;

  gbb = XNEW (struct gimple_bb);
  bb->aux = gbb;
  GBB_BB (gbb) = bb;
  GBB_DATA_REFS (gbb) = drs;
  GBB_CONDITIONS (gbb) = NULL;
  GBB_CONDITION_CASES (gbb) = NULL;

  return gbb;
}

static void
free_data_refs_aux (VEC (data_reference_p, heap) *datarefs)
{
  unsigned int i;
  struct data_reference *dr;

  FOR_EACH_VEC_ELT (data_reference_p, datarefs, i, dr)
    if (dr->aux)
      {
	base_alias_pair *bap = (base_alias_pair *)(dr->aux);

	free (bap->alias_set);

	free (bap);
	dr->aux = NULL;
      }
}
/* Frees GBB.  */

static void
free_gimple_bb (struct gimple_bb *gbb)
{
  free_data_refs_aux (GBB_DATA_REFS (gbb));
  free_data_refs (GBB_DATA_REFS (gbb));

  VEC_free (gimple, heap, GBB_CONDITIONS (gbb));
  VEC_free (gimple, heap, GBB_CONDITION_CASES (gbb));
  GBB_BB (gbb)->aux = 0;
  XDELETE (gbb);
}

/* Deletes all gimple bbs in SCOP.  */

static void
remove_gbbs_in_scop (scop_p scop)
{
  int i;
  poly_bb_p pbb;

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    free_gimple_bb (PBB_BLACK_BOX (pbb));
}

/* Deletes all scops in SCOPS.  */

void
free_scops (VEC (scop_p, heap) *scops)
{
  int i;
  scop_p scop;

  FOR_EACH_VEC_ELT (scop_p, scops, i, scop)
    {
      remove_gbbs_in_scop (scop);
      free_sese (SCOP_REGION (scop));
      free_scop (scop);
    }

  VEC_free (scop_p, heap, scops);
}

/* Same as outermost_loop_in_sese, returns the outermost loop
   containing BB in REGION, but makes sure that the returned loop
   belongs to the REGION, and so this returns the first loop in the
   REGION when the loop containing BB does not belong to REGION.  */

static loop_p
outermost_loop_in_sese_1 (sese region, basic_block bb)
{
  loop_p nest = outermost_loop_in_sese (region, bb);

  if (loop_in_sese_p (nest, region))
    return nest;

  /* When the basic block BB does not belong to a loop in the region,
     return the first loop in the region.  */
  nest = nest->inner;
  while (nest)
    if (loop_in_sese_p (nest, region))
      break;
    else
      nest = nest->next;

  gcc_assert (nest);
  return nest;
}

/* Generates a polyhedral black box only if the bb contains interesting
   information.  */

static gimple_bb_p
try_generate_gimple_bb (scop_p scop, basic_block bb)
{
  VEC (data_reference_p, heap) *drs = VEC_alloc (data_reference_p, heap, 5);
  sese region = SCOP_REGION (scop);
  loop_p nest = outermost_loop_in_sese_1 (region, bb);
  gimple_stmt_iterator gsi;

  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gimple stmt = gsi_stmt (gsi);
      loop_p loop;

      if (is_gimple_debug (stmt))
	continue;

      loop = loop_containing_stmt (stmt);
      if (!loop_in_sese_p (loop, region))
	loop = nest;

      graphite_find_data_references_in_stmt (nest, loop, stmt, &drs);
    }

  return new_gimple_bb (bb, drs);
}

/* Returns true if all predecessors of BB, that are not dominated by BB, are
   marked in MAP.  The predecessors dominated by BB are loop latches and will
   be handled after BB.  */

static bool
all_non_dominated_preds_marked_p (basic_block bb, sbitmap map)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, bb->preds)
    if (!TEST_BIT (map, e->src->index)
	&& !dominated_by_p (CDI_DOMINATORS, e->src, bb))
	return false;

  return true;
}

/* Compare the depth of two basic_block's P1 and P2.  */

static int
compare_bb_depths (const void *p1, const void *p2)
{
  const_basic_block const bb1 = *(const_basic_block const*)p1;
  const_basic_block const bb2 = *(const_basic_block const*)p2;
  int d1 = loop_depth (bb1->loop_father);
  int d2 = loop_depth (bb2->loop_father);

  if (d1 < d2)
    return 1;

  if (d1 > d2)
    return -1;

  return 0;
}

/* Sort the basic blocks from DOM such that the first are the ones at
   a deepest loop level.  */

static void
graphite_sort_dominated_info (VEC (basic_block, heap) *dom)
{
  VEC_qsort (basic_block, dom, compare_bb_depths);
}

/* Recursive helper function for build_scops_bbs.  */

static void
build_scop_bbs_1 (scop_p scop, sbitmap visited, basic_block bb)
{
  sese region = SCOP_REGION (scop);
  VEC (basic_block, heap) *dom;
  poly_bb_p pbb;

  if (TEST_BIT (visited, bb->index)
      || !bb_in_sese_p (bb, region))
    return;

  pbb = new_poly_bb (scop, try_generate_gimple_bb (scop, bb));
  VEC_safe_push (poly_bb_p, heap, SCOP_BBS (scop), pbb);
  SET_BIT (visited, bb->index);

  dom = get_dominated_by (CDI_DOMINATORS, bb);

  if (dom == NULL)
    return;

  graphite_sort_dominated_info (dom);

  while (!VEC_empty (basic_block, dom))
    {
      int i;
      basic_block dom_bb;

      FOR_EACH_VEC_ELT (basic_block, dom, i, dom_bb)
	if (all_non_dominated_preds_marked_p (dom_bb, visited))
	  {
	    build_scop_bbs_1 (scop, visited, dom_bb);
	    VEC_unordered_remove (basic_block, dom, i);
	    break;
	  }
    }

  VEC_free (basic_block, heap, dom);
}

/* Gather the basic blocks belonging to the SCOP.  */

static void
build_scop_bbs (scop_p scop)
{
  sbitmap visited = sbitmap_alloc (last_basic_block);
  sese region = SCOP_REGION (scop);

  sbitmap_zero (visited);
  build_scop_bbs_1 (scop, visited, SESE_ENTRY_BB (region));
  sbitmap_free (visited);
}

/* Converts the STATIC_SCHEDULE of PBB into a scattering polyhedron.
   We generate SCATTERING_DIMENSIONS scattering dimensions.

   CLooG 0.15.0 and previous versions require, that all
   scattering functions of one CloogProgram have the same number of
   scattering dimensions, therefore we allow to specify it.  This
   should be removed in future versions of CLooG.

   The scattering polyhedron consists of these dimensions: scattering,
   loop_iterators, parameters.

   Example:

   | scattering_dimensions = 5
   | used_scattering_dimensions = 3
   | nb_iterators = 1
   | scop_nb_params = 2
   |
   | Schedule:
   |   i
   | 4 5
   |
   | Scattering polyhedron:
   |
   | scattering: {s1, s2, s3, s4, s5}
   | loop_iterators: {i}
   | parameters: {p1, p2}
   |
   | s1  s2  s3  s4  s5  i   p1  p2  1
   | 1   0   0   0   0   0   0   0  -4  = 0
   | 0   1   0   0   0  -1   0   0   0  = 0
   | 0   0   1   0   0   0   0   0  -5  = 0  */

static void
build_pbb_scattering_polyhedrons (ppl_Linear_Expression_t static_schedule,
				  poly_bb_p pbb, int scattering_dimensions)
{
  int i;
  scop_p scop = PBB_SCOP (pbb);
  int nb_iterators = pbb_dim_iter_domain (pbb);
  int used_scattering_dimensions = nb_iterators * 2 + 1;
  int nb_params = scop_nb_params (scop);
  ppl_Coefficient_t c;
  ppl_dimension_type dim = scattering_dimensions + nb_iterators + nb_params;
  mpz_t v;

  gcc_assert (scattering_dimensions >= used_scattering_dimensions);

  mpz_init (v);
  ppl_new_Coefficient (&c);
  PBB_TRANSFORMED (pbb) = poly_scattering_new ();
  ppl_new_C_Polyhedron_from_space_dimension
    (&PBB_TRANSFORMED_SCATTERING (pbb), dim, 0);

  PBB_NB_SCATTERING_TRANSFORM (pbb) = scattering_dimensions;

  for (i = 0; i < scattering_dimensions; i++)
    {
      ppl_Constraint_t cstr;
      ppl_Linear_Expression_t expr;

      ppl_new_Linear_Expression_with_dimension (&expr, dim);
      mpz_set_si (v, 1);
      ppl_assign_Coefficient_from_mpz_t (c, v);
      ppl_Linear_Expression_add_to_coefficient (expr, i, c);

      /* Textual order inside this loop.  */
      if ((i % 2) == 0)
	{
	  ppl_Linear_Expression_coefficient (static_schedule, i / 2, c);
	  ppl_Coefficient_to_mpz_t (c, v);
	  mpz_neg (v, v);
	  ppl_assign_Coefficient_from_mpz_t (c, v);
	  ppl_Linear_Expression_add_to_inhomogeneous (expr, c);
	}

      /* Iterations of this loop.  */
      else /* if ((i % 2) == 1) */
	{
	  int loop = (i - 1) / 2;

	  mpz_set_si (v, -1);
	  ppl_assign_Coefficient_from_mpz_t (c, v);
	  ppl_Linear_Expression_add_to_coefficient
	    (expr, scattering_dimensions + loop, c);
	}

      ppl_new_Constraint (&cstr, expr, PPL_CONSTRAINT_TYPE_EQUAL);
      ppl_Polyhedron_add_constraint (PBB_TRANSFORMED_SCATTERING (pbb), cstr);
      ppl_delete_Linear_Expression (expr);
      ppl_delete_Constraint (cstr);
    }

  mpz_clear (v);
  ppl_delete_Coefficient (c);

  PBB_ORIGINAL (pbb) = poly_scattering_copy (PBB_TRANSFORMED (pbb));
}

/* Build for BB the static schedule.

   The static schedule is a Dewey numbering of the abstract syntax
   tree: http://en.wikipedia.org/wiki/Dewey_Decimal_Classification

   The following example informally defines the static schedule:

   A
   for (i: ...)
     {
       for (j: ...)
         {
           B
           C
         }

       for (k: ...)
         {
           D
           E
         }
     }
   F

   Static schedules for A to F:

     DEPTH
     0 1 2
   A 0
   B 1 0 0
   C 1 0 1
   D 1 1 0
   E 1 1 1
   F 2
*/

static void
build_scop_scattering (scop_p scop)
{
  int i;
  poly_bb_p pbb;
  gimple_bb_p previous_gbb = NULL;
  ppl_Linear_Expression_t static_schedule;
  ppl_Coefficient_t c;
  mpz_t v;

  mpz_init (v);
  ppl_new_Coefficient (&c);
  ppl_new_Linear_Expression (&static_schedule);

  /* We have to start schedules at 0 on the first component and
     because we cannot compare_prefix_loops against a previous loop,
     prefix will be equal to zero, and that index will be
     incremented before copying.  */
  mpz_set_si (v, -1);
  ppl_assign_Coefficient_from_mpz_t (c, v);
  ppl_Linear_Expression_add_to_coefficient (static_schedule, 0, c);

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    {
      gimple_bb_p gbb = PBB_BLACK_BOX (pbb);
      ppl_Linear_Expression_t common;
      int prefix;
      int nb_scat_dims = pbb_dim_iter_domain (pbb) * 2 + 1;

      if (previous_gbb)
	prefix = nb_common_loops (SCOP_REGION (scop), previous_gbb, gbb);
      else
	prefix = 0;

      previous_gbb = gbb;
      ppl_new_Linear_Expression_with_dimension (&common, prefix + 1);
      ppl_assign_Linear_Expression_from_Linear_Expression (common,
							   static_schedule);

      mpz_set_si (v, 1);
      ppl_assign_Coefficient_from_mpz_t (c, v);
      ppl_Linear_Expression_add_to_coefficient (common, prefix, c);
      ppl_assign_Linear_Expression_from_Linear_Expression (static_schedule,
							   common);

      build_pbb_scattering_polyhedrons (common, pbb, nb_scat_dims);

      ppl_delete_Linear_Expression (common);
    }

  mpz_clear (v);
  ppl_delete_Coefficient (c);
  ppl_delete_Linear_Expression (static_schedule);
}

/* Add the value K to the dimension D of the linear expression EXPR.  */

static void
add_value_to_dim (ppl_dimension_type d, ppl_Linear_Expression_t expr,
		  mpz_t k)
{
  mpz_t val;
  ppl_Coefficient_t coef;

  ppl_new_Coefficient (&coef);
  ppl_Linear_Expression_coefficient (expr, d, coef);
  mpz_init (val);
  ppl_Coefficient_to_mpz_t (coef, val);

  mpz_add (val, val, k);

  ppl_assign_Coefficient_from_mpz_t (coef, val);
  ppl_Linear_Expression_add_to_coefficient (expr, d, coef);
  mpz_clear (val);
  ppl_delete_Coefficient (coef);
}

/* In the context of scop S, scan E, the right hand side of a scalar
   evolution function in loop VAR, and translate it to a linear
   expression EXPR.  */

static void
scan_tree_for_params_right_scev (sese s, tree e, int var,
				 ppl_Linear_Expression_t expr)
{
  if (expr)
    {
      loop_p loop = get_loop (var);
      ppl_dimension_type l = sese_loop_depth (s, loop) - 1;
      mpz_t val;

      /* Scalar evolutions should happen in the sese region.  */
      gcc_assert (sese_loop_depth (s, loop) > 0);

      /* We can not deal with parametric strides like:

      | p = parameter;
      |
      | for i:
      |   a [i * p] = ...   */
      gcc_assert (TREE_CODE (e) == INTEGER_CST);

      mpz_init (val);
      tree_int_to_gmp (e, val);
      add_value_to_dim (l, expr, val);
      mpz_clear (val);
    }
}

/* Scan the integer constant CST, and add it to the inhomogeneous part of the
   linear expression EXPR.  K is the multiplier of the constant.  */

static void
scan_tree_for_params_int (tree cst, ppl_Linear_Expression_t expr, mpz_t k)
{
  mpz_t val;
  ppl_Coefficient_t coef;
  tree type = TREE_TYPE (cst);

  mpz_init (val);

  /* Necessary to not get "-1 = 2^n - 1". */
  mpz_set_double_int (val, double_int_sext (tree_to_double_int (cst),
					    TYPE_PRECISION (type)), false);

  mpz_mul (val, val, k);
  ppl_new_Coefficient (&coef);
  ppl_assign_Coefficient_from_mpz_t (coef, val);
  ppl_Linear_Expression_add_to_inhomogeneous (expr, coef);
  mpz_clear (val);
  ppl_delete_Coefficient (coef);
}

/* When parameter NAME is in REGION, returns its index in SESE_PARAMS.
   Otherwise returns -1.  */

static inline int
parameter_index_in_region_1 (tree name, sese region)
{
  int i;
  tree p;

  gcc_assert (TREE_CODE (name) == SSA_NAME);

  FOR_EACH_VEC_ELT (tree, SESE_PARAMS (region), i, p)
    if (p == name)
      return i;

  return -1;
}

/* When the parameter NAME is in REGION, returns its index in
   SESE_PARAMS.  Otherwise this function inserts NAME in SESE_PARAMS
   and returns the index of NAME.  */

static int
parameter_index_in_region (tree name, sese region)
{
  int i;

  gcc_assert (TREE_CODE (name) == SSA_NAME);

  i = parameter_index_in_region_1 (name, region);
  if (i != -1)
    return i;

  gcc_assert (SESE_ADD_PARAMS (region));

  i = VEC_length (tree, SESE_PARAMS (region));
  VEC_safe_push (tree, heap, SESE_PARAMS (region), name);
  return i;
}

/* In the context of sese S, scan the expression E and translate it to
   a linear expression C.  When parsing a symbolic multiplication, K
   represents the constant multiplier of an expression containing
   parameters.  */

static void
scan_tree_for_params (sese s, tree e, ppl_Linear_Expression_t c,
		      mpz_t k)
{
  if (e == chrec_dont_know)
    return;

  switch (TREE_CODE (e))
    {
    case POLYNOMIAL_CHREC:
      scan_tree_for_params_right_scev (s, CHREC_RIGHT (e),
				       CHREC_VARIABLE (e), c);
      scan_tree_for_params (s, CHREC_LEFT (e), c, k);
      break;

    case MULT_EXPR:
      if (chrec_contains_symbols (TREE_OPERAND (e, 0)))
	{
	  if (c)
	    {
	      mpz_t val;
	      gcc_assert (host_integerp (TREE_OPERAND (e, 1), 0));
	      mpz_init (val);
	      tree_int_to_gmp (TREE_OPERAND (e, 1), val);
	      mpz_mul (val, val, k);
	      scan_tree_for_params (s, TREE_OPERAND (e, 0), c, val);
	      mpz_clear (val);
	    }
	  else
	    scan_tree_for_params (s, TREE_OPERAND (e, 0), c, k);
	}
      else
	{
	  if (c)
	    {
	      mpz_t val;
	      gcc_assert (host_integerp (TREE_OPERAND (e, 0), 0));
	      mpz_init (val);
	      tree_int_to_gmp (TREE_OPERAND (e, 0), val);
	      mpz_mul (val, val, k);
	      scan_tree_for_params (s, TREE_OPERAND (e, 1), c, val);
	      mpz_clear (val);
	    }
	  else
	    scan_tree_for_params (s, TREE_OPERAND (e, 1), c, k);
	}
      break;

    case PLUS_EXPR:
    case POINTER_PLUS_EXPR:
      scan_tree_for_params (s, TREE_OPERAND (e, 0), c, k);
      scan_tree_for_params (s, TREE_OPERAND (e, 1), c, k);
      break;

    case MINUS_EXPR:
      {
	ppl_Linear_Expression_t tmp_expr = NULL;

        if (c)
	  {
	    ppl_dimension_type dim;
	    ppl_Linear_Expression_space_dimension (c, &dim);
	    ppl_new_Linear_Expression_with_dimension (&tmp_expr, dim);
	  }

	scan_tree_for_params (s, TREE_OPERAND (e, 0), c, k);
	scan_tree_for_params (s, TREE_OPERAND (e, 1), tmp_expr, k);

	if (c)
	  {
	    ppl_subtract_Linear_Expression_from_Linear_Expression (c,
								   tmp_expr);
	    ppl_delete_Linear_Expression (tmp_expr);
	  }

	break;
      }

    case NEGATE_EXPR:
      {
	ppl_Linear_Expression_t tmp_expr = NULL;

	if (c)
	  {
	    ppl_dimension_type dim;
	    ppl_Linear_Expression_space_dimension (c, &dim);
	    ppl_new_Linear_Expression_with_dimension (&tmp_expr, dim);
	  }

	scan_tree_for_params (s, TREE_OPERAND (e, 0), tmp_expr, k);

	if (c)
	  {
	    ppl_subtract_Linear_Expression_from_Linear_Expression (c,
								   tmp_expr);
	    ppl_delete_Linear_Expression (tmp_expr);
	  }

	break;
      }

    case BIT_NOT_EXPR:
      {
	ppl_Linear_Expression_t tmp_expr = NULL;

	if (c)
	  {
	    ppl_dimension_type dim;
	    ppl_Linear_Expression_space_dimension (c, &dim);
	    ppl_new_Linear_Expression_with_dimension (&tmp_expr, dim);
	  }

	scan_tree_for_params (s, TREE_OPERAND (e, 0), tmp_expr, k);

	if (c)
	  {
	    ppl_Coefficient_t coef;
	    mpz_t minus_one;

	    ppl_subtract_Linear_Expression_from_Linear_Expression (c,
								   tmp_expr);
	    ppl_delete_Linear_Expression (tmp_expr);
	    mpz_init (minus_one);
	    mpz_set_si (minus_one, -1);
	    ppl_new_Coefficient_from_mpz_t (&coef, minus_one);
	    ppl_Linear_Expression_add_to_inhomogeneous (c, coef);
	    mpz_clear (minus_one);
	    ppl_delete_Coefficient (coef);
	  }

	break;
      }

    case SSA_NAME:
      {
	ppl_dimension_type p = parameter_index_in_region (e, s);

	if (c)
	  {
	    ppl_dimension_type dim;
	    ppl_Linear_Expression_space_dimension (c, &dim);
	    p += dim - sese_nb_params (s);
	    add_value_to_dim (p, c, k);
	  }
	break;
      }

    case INTEGER_CST:
      if (c)
	scan_tree_for_params_int (e, c, k);
      break;

    CASE_CONVERT:
    case NON_LVALUE_EXPR:
      scan_tree_for_params (s, TREE_OPERAND (e, 0), c, k);
      break;

    case ADDR_EXPR:
      break;

   default:
      gcc_unreachable ();
      break;
    }
}

/* Find parameters with respect to REGION in BB. We are looking in memory
   access functions, conditions and loop bounds.  */

static void
find_params_in_bb (sese region, gimple_bb_p gbb)
{
  int i;
  unsigned j;
  data_reference_p dr;
  gimple stmt;
  loop_p loop = GBB_BB (gbb)->loop_father;
  mpz_t one;

  mpz_init (one);
  mpz_set_si (one, 1);

  /* Find parameters in the access functions of data references.  */
  FOR_EACH_VEC_ELT (data_reference_p, GBB_DATA_REFS (gbb), i, dr)
    for (j = 0; j < DR_NUM_DIMENSIONS (dr); j++)
      scan_tree_for_params (region, DR_ACCESS_FN (dr, j), NULL, one);

  /* Find parameters in conditional statements.  */
  FOR_EACH_VEC_ELT (gimple, GBB_CONDITIONS (gbb), i, stmt)
    {
      tree lhs = scalar_evolution_in_region (region, loop,
					     gimple_cond_lhs (stmt));
      tree rhs = scalar_evolution_in_region (region, loop,
					     gimple_cond_rhs (stmt));

      scan_tree_for_params (region, lhs, NULL, one);
      scan_tree_for_params (region, rhs, NULL, one);
    }

  mpz_clear (one);
}

/* Record the parameters used in the SCOP.  A variable is a parameter
   in a scop if it does not vary during the execution of that scop.  */

static void
find_scop_parameters (scop_p scop)
{
  poly_bb_p pbb;
  unsigned i;
  sese region = SCOP_REGION (scop);
  struct loop *loop;
  mpz_t one;

  mpz_init (one);
  mpz_set_si (one, 1);

  /* Find the parameters used in the loop bounds.  */
  FOR_EACH_VEC_ELT (loop_p, SESE_LOOP_NEST (region), i, loop)
    {
      tree nb_iters = number_of_latch_executions (loop);

      if (!chrec_contains_symbols (nb_iters))
	continue;

      nb_iters = scalar_evolution_in_region (region, loop, nb_iters);
      scan_tree_for_params (region, nb_iters, NULL, one);
    }

  mpz_clear (one);

  /* Find the parameters used in data accesses.  */
  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    find_params_in_bb (region, PBB_BLACK_BOX (pbb));

  scop_set_nb_params (scop, sese_nb_params (region));
  SESE_ADD_PARAMS (region) = false;

  ppl_new_Pointset_Powerset_C_Polyhedron_from_space_dimension
    (&SCOP_CONTEXT (scop), scop_nb_params (scop), 0);
}

/* Insert in the SCOP context constraints from the estimation of the
   number of iterations.  UB_EXPR is a linear expression describing
   the number of iterations in a loop.  This expression is bounded by
   the estimation NIT.  */

static void
add_upper_bounds_from_estimated_nit (scop_p scop, double_int nit,
				     ppl_dimension_type dim,
				     ppl_Linear_Expression_t ub_expr)
{
  mpz_t val;
  ppl_Linear_Expression_t nb_iters_le;
  ppl_Polyhedron_t pol;
  ppl_Coefficient_t coef;
  ppl_Constraint_t ub;

  ppl_new_C_Polyhedron_from_space_dimension (&pol, dim, 0);
  ppl_new_Linear_Expression_from_Linear_Expression (&nb_iters_le,
						    ub_expr);

  /* Construct the negated number of last iteration in VAL.  */
  mpz_init (val);
  mpz_set_double_int (val, nit, false);
  mpz_sub_ui (val, val, 1);
  mpz_neg (val, val);

  /* NB_ITERS_LE holds the number of last iteration in
     parametrical form.  Subtract estimated number of last
     iteration and assert that result is not positive.  */
  ppl_new_Coefficient_from_mpz_t (&coef, val);
  ppl_Linear_Expression_add_to_inhomogeneous (nb_iters_le, coef);
  ppl_delete_Coefficient (coef);
  ppl_new_Constraint (&ub, nb_iters_le,
		      PPL_CONSTRAINT_TYPE_LESS_OR_EQUAL);
  ppl_Polyhedron_add_constraint (pol, ub);

  /* Remove all but last GDIM dimensions from POL to obtain
     only the constraints on the parameters.  */
  {
    graphite_dim_t gdim = scop_nb_params (scop);
    ppl_dimension_type *dims = XNEWVEC (ppl_dimension_type, dim - gdim);
    graphite_dim_t i;

    for (i = 0; i < dim - gdim; i++)
      dims[i] = i;

    ppl_Polyhedron_remove_space_dimensions (pol, dims, dim - gdim);
    XDELETEVEC (dims);
  }

  /* Add the constraints on the parameters to the SCoP context.  */
  {
    ppl_Pointset_Powerset_C_Polyhedron_t constraints_ps;

    ppl_new_Pointset_Powerset_C_Polyhedron_from_C_Polyhedron
      (&constraints_ps, pol);
    ppl_Pointset_Powerset_C_Polyhedron_intersection_assign
      (SCOP_CONTEXT (scop), constraints_ps);
    ppl_delete_Pointset_Powerset_C_Polyhedron (constraints_ps);
  }

  ppl_delete_Polyhedron (pol);
  ppl_delete_Linear_Expression (nb_iters_le);
  ppl_delete_Constraint (ub);
  mpz_clear (val);
}

/* Builds the constraint polyhedra for LOOP in SCOP.  OUTER_PH gives
   the constraints for the surrounding loops.  */

static void
build_loop_iteration_domains (scop_p scop, struct loop *loop,
                              ppl_Polyhedron_t outer_ph, int nb,
			      ppl_Pointset_Powerset_C_Polyhedron_t *domains)
{
  int i;
  ppl_Polyhedron_t ph;
  tree nb_iters = number_of_latch_executions (loop);
  ppl_dimension_type dim = nb + 1 + scop_nb_params (scop);
  sese region = SCOP_REGION (scop);

  {
    ppl_const_Constraint_System_t pcs;
    ppl_dimension_type *map
      = (ppl_dimension_type *) XNEWVEC (ppl_dimension_type, dim);

    ppl_new_C_Polyhedron_from_space_dimension (&ph, dim, 0);
    ppl_Polyhedron_get_constraints (outer_ph, &pcs);
    ppl_Polyhedron_add_constraints (ph, pcs);

    for (i = 0; i < (int) nb; i++)
      map[i] = i;
    for (i = (int) nb; i < (int) dim - 1; i++)
      map[i] = i + 1;
    map[dim - 1] = nb;

    ppl_Polyhedron_map_space_dimensions (ph, map, dim);
    free (map);
  }

  /* 0 <= loop_i */
  {
    ppl_Constraint_t lb;
    ppl_Linear_Expression_t lb_expr;

    ppl_new_Linear_Expression_with_dimension (&lb_expr, dim);
    ppl_set_coef (lb_expr, nb, 1);
    ppl_new_Constraint (&lb, lb_expr, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
    ppl_delete_Linear_Expression (lb_expr);
    ppl_Polyhedron_add_constraint (ph, lb);
    ppl_delete_Constraint (lb);
  }

  if (TREE_CODE (nb_iters) == INTEGER_CST)
    {
      ppl_Constraint_t ub;
      ppl_Linear_Expression_t ub_expr;

      ppl_new_Linear_Expression_with_dimension (&ub_expr, dim);

      /* loop_i <= cst_nb_iters */
      ppl_set_coef (ub_expr, nb, -1);
      ppl_set_inhomogeneous_tree (ub_expr, nb_iters);
      ppl_new_Constraint (&ub, ub_expr, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
      ppl_Polyhedron_add_constraint (ph, ub);
      ppl_delete_Linear_Expression (ub_expr);
      ppl_delete_Constraint (ub);
    }
  else if (!chrec_contains_undetermined (nb_iters))
    {
      mpz_t one;
      ppl_Constraint_t ub;
      ppl_Linear_Expression_t ub_expr;
      double_int nit;

      mpz_init (one);
      mpz_set_si (one, 1);
      ppl_new_Linear_Expression_with_dimension (&ub_expr, dim);
      nb_iters = scalar_evolution_in_region (region, loop, nb_iters);
      scan_tree_for_params (SCOP_REGION (scop), nb_iters, ub_expr, one);
      mpz_clear (one);

      if (max_stmt_executions (loop, true, &nit))
	add_upper_bounds_from_estimated_nit (scop, nit, dim, ub_expr);

      /* loop_i <= expr_nb_iters */
      ppl_set_coef (ub_expr, nb, -1);
      ppl_new_Constraint (&ub, ub_expr, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
      ppl_Polyhedron_add_constraint (ph, ub);
      ppl_delete_Linear_Expression (ub_expr);
      ppl_delete_Constraint (ub);
    }
  else
    gcc_unreachable ();

  if (loop->inner && loop_in_sese_p (loop->inner, region))
    build_loop_iteration_domains (scop, loop->inner, ph, nb + 1, domains);

  if (nb != 0
      && loop->next
      && loop_in_sese_p (loop->next, region))
    build_loop_iteration_domains (scop, loop->next, outer_ph, nb, domains);

  ppl_new_Pointset_Powerset_C_Polyhedron_from_C_Polyhedron
    (&domains[loop->num], ph);

  ppl_delete_Polyhedron (ph);
}

/* Returns a linear expression for tree T evaluated in PBB.  */

static ppl_Linear_Expression_t
create_linear_expr_from_tree (poly_bb_p pbb, tree t)
{
  mpz_t one;
  ppl_Linear_Expression_t res;
  ppl_dimension_type dim;
  sese region = SCOP_REGION (PBB_SCOP (pbb));
  loop_p loop = pbb_loop (pbb);

  dim = pbb_dim_iter_domain (pbb) + pbb_nb_params (pbb);
  ppl_new_Linear_Expression_with_dimension (&res, dim);

  t = scalar_evolution_in_region (region, loop, t);
  gcc_assert (!automatically_generated_chrec_p (t));

  mpz_init (one);
  mpz_set_si (one, 1);
  scan_tree_for_params (region, t, res, one);
  mpz_clear (one);

  return res;
}

/* Returns the ppl constraint type from the gimple tree code CODE.  */

static enum ppl_enum_Constraint_Type
ppl_constraint_type_from_tree_code (enum tree_code code)
{
  switch (code)
    {
    /* We do not support LT and GT to be able to work with C_Polyhedron.
       As we work on integer polyhedron "a < b" can be expressed by
       "a + 1 <= b".  */
    case LT_EXPR:
    case GT_EXPR:
      gcc_unreachable ();

    case LE_EXPR:
      return PPL_CONSTRAINT_TYPE_LESS_OR_EQUAL;

    case GE_EXPR:
      return PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL;

    case EQ_EXPR:
      return PPL_CONSTRAINT_TYPE_EQUAL;

    default:
      gcc_unreachable ();
    }
}

/* Add conditional statement STMT to PS.  It is evaluated in PBB and
   CODE is used as the comparison operator.  This allows us to invert the
   condition or to handle inequalities.  */

static void
add_condition_to_domain (ppl_Pointset_Powerset_C_Polyhedron_t ps, gimple stmt,
			 poly_bb_p pbb, enum tree_code code)
{
  mpz_t v;
  ppl_Coefficient_t c;
  ppl_Linear_Expression_t left, right;
  ppl_Constraint_t cstr;
  enum ppl_enum_Constraint_Type type;

  left = create_linear_expr_from_tree (pbb, gimple_cond_lhs (stmt));
  right = create_linear_expr_from_tree (pbb, gimple_cond_rhs (stmt));

  /* If we have < or > expressions convert them to <= or >= by adding 1 to
     the left or the right side of the expression. */
  if (code == LT_EXPR)
    {
      mpz_init (v);
      mpz_set_si (v, 1);
      ppl_new_Coefficient (&c);
      ppl_assign_Coefficient_from_mpz_t (c, v);
      ppl_Linear_Expression_add_to_inhomogeneous (left, c);
      ppl_delete_Coefficient (c);
      mpz_clear (v);

      code = LE_EXPR;
    }
  else if (code == GT_EXPR)
    {
      mpz_init (v);
      mpz_set_si (v, 1);
      ppl_new_Coefficient (&c);
      ppl_assign_Coefficient_from_mpz_t (c, v);
      ppl_Linear_Expression_add_to_inhomogeneous (right, c);
      ppl_delete_Coefficient (c);
      mpz_clear (v);

      code = GE_EXPR;
    }

  type = ppl_constraint_type_from_tree_code (code);

  ppl_subtract_Linear_Expression_from_Linear_Expression (left, right);

  ppl_new_Constraint (&cstr, left, type);
  ppl_Pointset_Powerset_C_Polyhedron_add_constraint (ps, cstr);

  ppl_delete_Constraint (cstr);
  ppl_delete_Linear_Expression (left);
  ppl_delete_Linear_Expression (right);
}

/* Add conditional statement STMT to pbb.  CODE is used as the comparision
   operator.  This allows us to invert the condition or to handle
   inequalities.  */

static void
add_condition_to_pbb (poly_bb_p pbb, gimple stmt, enum tree_code code)
{
  if (code == NE_EXPR)
    {
      ppl_Pointset_Powerset_C_Polyhedron_t left = PBB_DOMAIN (pbb);
      ppl_Pointset_Powerset_C_Polyhedron_t right;
      ppl_new_Pointset_Powerset_C_Polyhedron_from_Pointset_Powerset_C_Polyhedron
	(&right, left);
      add_condition_to_domain (left, stmt, pbb, LT_EXPR);
      add_condition_to_domain (right, stmt, pbb, GT_EXPR);
      ppl_Pointset_Powerset_C_Polyhedron_upper_bound_assign (left, right);
      ppl_delete_Pointset_Powerset_C_Polyhedron (right);
    }
  else
    add_condition_to_domain (PBB_DOMAIN (pbb), stmt, pbb, code);
}

/* Add conditions to the domain of PBB.  */

static void
add_conditions_to_domain (poly_bb_p pbb)
{
  unsigned int i;
  gimple stmt;
  gimple_bb_p gbb = PBB_BLACK_BOX (pbb);

  if (VEC_empty (gimple, GBB_CONDITIONS (gbb)))
    return;

  FOR_EACH_VEC_ELT (gimple, GBB_CONDITIONS (gbb), i, stmt)
    switch (gimple_code (stmt))
      {
      case GIMPLE_COND:
	  {
	    enum tree_code code = gimple_cond_code (stmt);

	    /* The conditions for ELSE-branches are inverted.  */
	    if (!VEC_index (gimple, GBB_CONDITION_CASES (gbb), i))
	      code = invert_tree_comparison (code, false);

	    add_condition_to_pbb (pbb, stmt, code);
	    break;
	  }

      case GIMPLE_SWITCH:
	/* Switch statements are not supported right now - fall throught.  */

      default:
	gcc_unreachable ();
	break;
      }
}

/* Traverses all the GBBs of the SCOP and add their constraints to the
   iteration domains.  */

static void
add_conditions_to_constraints (scop_p scop)
{
  int i;
  poly_bb_p pbb;

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    add_conditions_to_domain (pbb);
}

/* Structure used to pass data to dom_walk.  */

struct bsc
{
  VEC (gimple, heap) **conditions, **cases;
  sese region;
};

/* Returns a COND_EXPR statement when BB has a single predecessor, the
   edge between BB and its predecessor is not a loop exit edge, and
   the last statement of the single predecessor is a COND_EXPR.  */

static gimple
single_pred_cond_non_loop_exit (basic_block bb)
{
  if (single_pred_p (bb))
    {
      edge e = single_pred_edge (bb);
      basic_block pred = e->src;
      gimple stmt;

      if (loop_depth (pred->loop_father) > loop_depth (bb->loop_father))
	return NULL;

      stmt = last_stmt (pred);

      if (stmt && gimple_code (stmt) == GIMPLE_COND)
	return stmt;
    }

  return NULL;
}

/* Call-back for dom_walk executed before visiting the dominated
   blocks.  */

static void
build_sese_conditions_before (struct dom_walk_data *dw_data,
			      basic_block bb)
{
  struct bsc *data = (struct bsc *) dw_data->global_data;
  VEC (gimple, heap) **conditions = data->conditions;
  VEC (gimple, heap) **cases = data->cases;
  gimple_bb_p gbb;
  gimple stmt;

  if (!bb_in_sese_p (bb, data->region))
    return;

  stmt = single_pred_cond_non_loop_exit (bb);

  if (stmt)
    {
      edge e = single_pred_edge (bb);

      VEC_safe_push (gimple, heap, *conditions, stmt);

      if (e->flags & EDGE_TRUE_VALUE)
	VEC_safe_push (gimple, heap, *cases, stmt);
      else
	VEC_safe_push (gimple, heap, *cases, NULL);
    }

  gbb = gbb_from_bb (bb);

  if (gbb)
    {
      GBB_CONDITIONS (gbb) = VEC_copy (gimple, heap, *conditions);
      GBB_CONDITION_CASES (gbb) = VEC_copy (gimple, heap, *cases);
    }
}

/* Call-back for dom_walk executed after visiting the dominated
   blocks.  */

static void
build_sese_conditions_after (struct dom_walk_data *dw_data,
			     basic_block bb)
{
  struct bsc *data = (struct bsc *) dw_data->global_data;
  VEC (gimple, heap) **conditions = data->conditions;
  VEC (gimple, heap) **cases = data->cases;

  if (!bb_in_sese_p (bb, data->region))
    return;

  if (single_pred_cond_non_loop_exit (bb))
    {
      VEC_pop (gimple, *conditions);
      VEC_pop (gimple, *cases);
    }
}

/* Record all conditions in REGION.  */

static void
build_sese_conditions (sese region)
{
  struct dom_walk_data walk_data;
  VEC (gimple, heap) *conditions = VEC_alloc (gimple, heap, 3);
  VEC (gimple, heap) *cases = VEC_alloc (gimple, heap, 3);
  struct bsc data;

  data.conditions = &conditions;
  data.cases = &cases;
  data.region = region;

  walk_data.dom_direction = CDI_DOMINATORS;
  walk_data.initialize_block_local_data = NULL;
  walk_data.before_dom_children = build_sese_conditions_before;
  walk_data.after_dom_children = build_sese_conditions_after;
  walk_data.global_data = &data;
  walk_data.block_local_data_size = 0;

  init_walk_dominator_tree (&walk_data);
  walk_dominator_tree (&walk_data, SESE_ENTRY_BB (region));
  fini_walk_dominator_tree (&walk_data);

  VEC_free (gimple, heap, conditions);
  VEC_free (gimple, heap, cases);
}

/* Add constraints on the possible values of parameter P from the type
   of P.  */

static void
add_param_constraints (scop_p scop, ppl_Polyhedron_t context, graphite_dim_t p)
{
  ppl_Constraint_t cstr;
  ppl_Linear_Expression_t le;
  tree parameter = VEC_index (tree, SESE_PARAMS (SCOP_REGION (scop)), p);
  tree type = TREE_TYPE (parameter);
  tree lb = NULL_TREE;
  tree ub = NULL_TREE;

  if (POINTER_TYPE_P (type) || !TYPE_MIN_VALUE (type))
    lb = lower_bound_in_type (type, type);
  else
    lb = TYPE_MIN_VALUE (type);

  if (POINTER_TYPE_P (type) || !TYPE_MAX_VALUE (type))
    ub = upper_bound_in_type (type, type);
  else
    ub = TYPE_MAX_VALUE (type);

  if (lb)
    {
      ppl_new_Linear_Expression_with_dimension (&le, scop_nb_params (scop));
      ppl_set_coef (le, p, -1);
      ppl_set_inhomogeneous_tree (le, lb);
      ppl_new_Constraint (&cstr, le, PPL_CONSTRAINT_TYPE_LESS_OR_EQUAL);
      ppl_Polyhedron_add_constraint (context, cstr);
      ppl_delete_Linear_Expression (le);
      ppl_delete_Constraint (cstr);
    }

  if (ub)
    {
      ppl_new_Linear_Expression_with_dimension (&le, scop_nb_params (scop));
      ppl_set_coef (le, p, -1);
      ppl_set_inhomogeneous_tree (le, ub);
      ppl_new_Constraint (&cstr, le, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
      ppl_Polyhedron_add_constraint (context, cstr);
      ppl_delete_Linear_Expression (le);
      ppl_delete_Constraint (cstr);
    }
}

/* Build the context of the SCOP.  The context usually contains extra
   constraints that are added to the iteration domains that constrain
   some parameters.  */

static void
build_scop_context (scop_p scop)
{
  ppl_Polyhedron_t context;
  ppl_Pointset_Powerset_C_Polyhedron_t ps;
  graphite_dim_t p, n = scop_nb_params (scop);

  ppl_new_C_Polyhedron_from_space_dimension (&context, n, 0);

  for (p = 0; p < n; p++)
    add_param_constraints (scop, context, p);

  ppl_new_Pointset_Powerset_C_Polyhedron_from_C_Polyhedron
    (&ps, context);
  ppl_Pointset_Powerset_C_Polyhedron_intersection_assign
    (SCOP_CONTEXT (scop), ps);

  ppl_delete_Pointset_Powerset_C_Polyhedron (ps);
  ppl_delete_Polyhedron (context);
}

/* Build the iteration domains: the loops belonging to the current
   SCOP, and that vary for the execution of the current basic block.
   Returns false if there is no loop in SCOP.  */

static void
build_scop_iteration_domain (scop_p scop)
{
  struct loop *loop;
  sese region = SCOP_REGION (scop);
  int i;
  ppl_Polyhedron_t ph;
  poly_bb_p pbb;
  int nb_loops = number_of_loops ();
  ppl_Pointset_Powerset_C_Polyhedron_t *domains
    = XNEWVEC (ppl_Pointset_Powerset_C_Polyhedron_t, nb_loops);

  for (i = 0; i < nb_loops; i++)
    domains[i] = NULL;

  ppl_new_C_Polyhedron_from_space_dimension (&ph, scop_nb_params (scop), 0);

  FOR_EACH_VEC_ELT (loop_p, SESE_LOOP_NEST (region), i, loop)
    if (!loop_in_sese_p (loop_outer (loop), region))
      build_loop_iteration_domains (scop, loop, ph, 0, domains);

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    if (domains[gbb_loop (PBB_BLACK_BOX (pbb))->num])
      ppl_new_Pointset_Powerset_C_Polyhedron_from_Pointset_Powerset_C_Polyhedron
	(&PBB_DOMAIN (pbb), (ppl_const_Pointset_Powerset_C_Polyhedron_t)
	 domains[gbb_loop (PBB_BLACK_BOX (pbb))->num]);
    else
      ppl_new_Pointset_Powerset_C_Polyhedron_from_C_Polyhedron
	(&PBB_DOMAIN (pbb), ph);

  for (i = 0; i < nb_loops; i++)
    if (domains[i])
      ppl_delete_Pointset_Powerset_C_Polyhedron (domains[i]);

  ppl_delete_Polyhedron (ph);
  free (domains);
}

/* Add a constrain to the ACCESSES polyhedron for the alias set of
   data reference DR.  ACCESSP_NB_DIMS is the dimension of the
   ACCESSES polyhedron, DOM_NB_DIMS is the dimension of the iteration
   domain.  */

static void
pdr_add_alias_set (ppl_Polyhedron_t accesses, data_reference_p dr,
		   ppl_dimension_type accessp_nb_dims,
		   ppl_dimension_type dom_nb_dims)
{
  ppl_Linear_Expression_t alias;
  ppl_Constraint_t cstr;
  int alias_set_num = 0;
  base_alias_pair *bap = (base_alias_pair *)(dr->aux);

  if (bap && bap->alias_set)
    alias_set_num = *(bap->alias_set);

  ppl_new_Linear_Expression_with_dimension (&alias, accessp_nb_dims);

  ppl_set_coef (alias, dom_nb_dims, 1);
  ppl_set_inhomogeneous (alias, -alias_set_num);
  ppl_new_Constraint (&cstr, alias, PPL_CONSTRAINT_TYPE_EQUAL);
  ppl_Polyhedron_add_constraint (accesses, cstr);

  ppl_delete_Linear_Expression (alias);
  ppl_delete_Constraint (cstr);
}

/* Add to ACCESSES polyhedron equalities defining the access functions
   to the memory.  ACCESSP_NB_DIMS is the dimension of the ACCESSES
   polyhedron, DOM_NB_DIMS is the dimension of the iteration domain.
   PBB is the poly_bb_p that contains the data reference DR.  */

static void
pdr_add_memory_accesses (ppl_Polyhedron_t accesses, data_reference_p dr,
			 ppl_dimension_type accessp_nb_dims,
			 ppl_dimension_type dom_nb_dims,
			 poly_bb_p pbb)
{
  int i, nb_subscripts = DR_NUM_DIMENSIONS (dr);
  mpz_t v;
  scop_p scop = PBB_SCOP (pbb);
  sese region = SCOP_REGION (scop);

  mpz_init (v);

  for (i = 0; i < nb_subscripts; i++)
    {
      ppl_Linear_Expression_t fn, access;
      ppl_Constraint_t cstr;
      ppl_dimension_type subscript = dom_nb_dims + 1 + i;
      tree afn = DR_ACCESS_FN (dr, nb_subscripts - 1 - i);

      ppl_new_Linear_Expression_with_dimension (&fn, dom_nb_dims);
      ppl_new_Linear_Expression_with_dimension (&access, accessp_nb_dims);

      mpz_set_si (v, 1);
      scan_tree_for_params (region, afn, fn, v);
      ppl_assign_Linear_Expression_from_Linear_Expression (access, fn);

      ppl_set_coef (access, subscript, -1);
      ppl_new_Constraint (&cstr, access, PPL_CONSTRAINT_TYPE_EQUAL);
      ppl_Polyhedron_add_constraint (accesses, cstr);

      ppl_delete_Linear_Expression (fn);
      ppl_delete_Linear_Expression (access);
      ppl_delete_Constraint (cstr);
    }

  mpz_clear (v);
}

/* Add constrains representing the size of the accessed data to the
   ACCESSES polyhedron.  ACCESSP_NB_DIMS is the dimension of the
   ACCESSES polyhedron, DOM_NB_DIMS is the dimension of the iteration
   domain.  */

static void
pdr_add_data_dimensions (ppl_Polyhedron_t accesses, data_reference_p dr,
			 ppl_dimension_type accessp_nb_dims,
			 ppl_dimension_type dom_nb_dims)
{
  tree ref = DR_REF (dr);
  int i, nb_subscripts = DR_NUM_DIMENSIONS (dr);

  for (i = nb_subscripts - 1; i >= 0; i--, ref = TREE_OPERAND (ref, 0))
    {
      ppl_Linear_Expression_t expr;
      ppl_Constraint_t cstr;
      ppl_dimension_type subscript = dom_nb_dims + 1 + i;
      tree low, high;

      if (TREE_CODE (ref) != ARRAY_REF)
	break;

      low = array_ref_low_bound (ref);

      /* subscript - low >= 0 */
      if (host_integerp (low, 0))
	{
	  tree minus_low;

	  ppl_new_Linear_Expression_with_dimension (&expr, accessp_nb_dims);
	  ppl_set_coef (expr, subscript, 1);

	  minus_low = fold_build1 (NEGATE_EXPR, TREE_TYPE (low), low);
	  ppl_set_inhomogeneous_tree (expr, minus_low);

	  ppl_new_Constraint (&cstr, expr, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
	  ppl_Polyhedron_add_constraint (accesses, cstr);
	  ppl_delete_Linear_Expression (expr);
	  ppl_delete_Constraint (cstr);
	}

      high = array_ref_up_bound (ref);

      /* high - subscript >= 0 */
      if (high && host_integerp (high, 0)
	  /* 1-element arrays at end of structures may extend over
	     their declared size.  */
	  && !(array_at_struct_end_p (ref)
	       && operand_equal_p (low, high, 0)))
	{
	  ppl_new_Linear_Expression_with_dimension (&expr, accessp_nb_dims);
	  ppl_set_coef (expr, subscript, -1);

	  ppl_set_inhomogeneous_tree (expr, high);

	  ppl_new_Constraint (&cstr, expr, PPL_CONSTRAINT_TYPE_GREATER_OR_EQUAL);
	  ppl_Polyhedron_add_constraint (accesses, cstr);
	  ppl_delete_Linear_Expression (expr);
	  ppl_delete_Constraint (cstr);
	}
    }
}

/* Build data accesses for DR in PBB.  */

static void
build_poly_dr (data_reference_p dr, poly_bb_p pbb)
{
  ppl_Polyhedron_t accesses;
  ppl_Pointset_Powerset_C_Polyhedron_t accesses_ps;
  ppl_dimension_type dom_nb_dims;
  ppl_dimension_type accessp_nb_dims;
  int dr_base_object_set;

  ppl_Pointset_Powerset_C_Polyhedron_space_dimension (PBB_DOMAIN (pbb),
						      &dom_nb_dims);
  accessp_nb_dims = dom_nb_dims + 1 + DR_NUM_DIMENSIONS (dr);

  ppl_new_C_Polyhedron_from_space_dimension (&accesses, accessp_nb_dims, 0);

  pdr_add_alias_set (accesses, dr, accessp_nb_dims, dom_nb_dims);
  pdr_add_memory_accesses (accesses, dr, accessp_nb_dims, dom_nb_dims, pbb);
  pdr_add_data_dimensions (accesses, dr, accessp_nb_dims, dom_nb_dims);

  ppl_new_Pointset_Powerset_C_Polyhedron_from_C_Polyhedron (&accesses_ps,
							    accesses);
  ppl_delete_Polyhedron (accesses);

  gcc_assert (dr->aux);
  dr_base_object_set = ((base_alias_pair *)(dr->aux))->base_obj_set;

  new_poly_dr (pbb, dr_base_object_set, accesses_ps,
	       DR_IS_READ (dr) ? PDR_READ : PDR_WRITE,
	       dr, DR_NUM_DIMENSIONS (dr));
}

/* Write to FILE the alias graph of data references in DIMACS format.  */

static inline bool
write_alias_graph_to_ascii_dimacs (FILE *file, char *comment,
				   VEC (data_reference_p, heap) *drs)
{
  int num_vertex = VEC_length (data_reference_p, drs);
  int edge_num = 0;
  data_reference_p dr1, dr2;
  int i, j;

  if (num_vertex == 0)
    return true;

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i + 1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_may_alias_p (dr1, dr2, true))
	edge_num++;

  fprintf (file, "$\n");

  if (comment)
    fprintf (file, "c %s\n", comment);

  fprintf (file, "p edge %d %d\n", num_vertex, edge_num);

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i + 1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_may_alias_p (dr1, dr2, true))
	fprintf (file, "e %d %d\n", i + 1, j + 1);

  return true;
}

/* Write to FILE the alias graph of data references in DOT format.  */

static inline bool
write_alias_graph_to_ascii_dot (FILE *file, char *comment,
				VEC (data_reference_p, heap) *drs)
{
  int num_vertex = VEC_length (data_reference_p, drs);
  data_reference_p dr1, dr2;
  int i, j;

  if (num_vertex == 0)
    return true;

  fprintf (file, "$\n");

  if (comment)
    fprintf (file, "c %s\n", comment);

  /* First print all the vertices.  */
  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    fprintf (file, "n%d;\n", i);

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i + 1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_may_alias_p (dr1, dr2, true))
	fprintf (file, "n%d n%d\n", i, j);

  return true;
}

/* Write to FILE the alias graph of data references in ECC format.  */

static inline bool
write_alias_graph_to_ascii_ecc (FILE *file, char *comment,
				VEC (data_reference_p, heap) *drs)
{
  int num_vertex = VEC_length (data_reference_p, drs);
  data_reference_p dr1, dr2;
  int i, j;

  if (num_vertex == 0)
    return true;

  fprintf (file, "$\n");

  if (comment)
    fprintf (file, "c %s\n", comment);

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i + 1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_may_alias_p (dr1, dr2, true))
	fprintf (file, "%d %d\n", i, j);

  return true;
}

/* Check if DR1 and DR2 are in the same object set.  */

static bool
dr_same_base_object_p (const struct data_reference *dr1,
		       const struct data_reference *dr2)
{
  return operand_equal_p (DR_BASE_OBJECT (dr1), DR_BASE_OBJECT (dr2), 0);
}

/* Uses DFS component number as representative of alias-sets. Also tests for
   optimality by verifying if every connected component is a clique. Returns
   true (1) if the above test is true, and false (0) otherwise.  */

static int
build_alias_set_optimal_p (VEC (data_reference_p, heap) *drs)
{
  int num_vertices = VEC_length (data_reference_p, drs);
  struct graph *g = new_graph (num_vertices);
  data_reference_p dr1, dr2;
  int i, j;
  int num_connected_components;
  int v_indx1, v_indx2, num_vertices_in_component;
  int *all_vertices;
  int *vertices;
  struct graph_edge *e;
  int this_component_is_clique;
  int all_components_are_cliques = 1;

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i+1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_may_alias_p (dr1, dr2, true))
	{
	  add_edge (g, i, j);
	  add_edge (g, j, i);
	}

  all_vertices = XNEWVEC (int, num_vertices);
  vertices = XNEWVEC (int, num_vertices);
  for (i = 0; i < num_vertices; i++)
    all_vertices[i] = i;

  num_connected_components = graphds_dfs (g, all_vertices, num_vertices,
					  NULL, true, NULL);
  for (i = 0; i < g->n_vertices; i++)
    {
      data_reference_p dr = VEC_index (data_reference_p, drs, i);
      base_alias_pair *bap;

      gcc_assert (dr->aux);
      bap = (base_alias_pair *)(dr->aux);

      bap->alias_set = XNEW (int);
      *(bap->alias_set) = g->vertices[i].component + 1;
    }

  /* Verify if the DFS numbering results in optimal solution.  */
  for (i = 0; i < num_connected_components; i++)
    {
      num_vertices_in_component = 0;
      /* Get all vertices whose DFS component number is the same as i.  */
      for (j = 0; j < num_vertices; j++)
	if (g->vertices[j].component == i)
	  vertices[num_vertices_in_component++] = j;

      /* Now test if the vertices in 'vertices' form a clique, by testing
	 for edges among each pair.  */
      this_component_is_clique = 1;
      for (v_indx1 = 0; v_indx1 < num_vertices_in_component; v_indx1++)
	{
	  for (v_indx2 = v_indx1+1; v_indx2 < num_vertices_in_component; v_indx2++)
	    {
	      /* Check if the two vertices are connected by iterating
		 through all the edges which have one of these are source.  */
	      e = g->vertices[vertices[v_indx2]].pred;
	      while (e)
		{
		  if (e->src == vertices[v_indx1])
		    break;
		  e = e->pred_next;
		}
	      if (!e)
		{
		  this_component_is_clique = 0;
		  break;
		}
	    }
	  if (!this_component_is_clique)
	    all_components_are_cliques = 0;
	}
    }

  free (all_vertices);
  free (vertices);
  free_graph (g);
  return all_components_are_cliques;
}

/* Group each data reference in DRS with its base object set num.  */

static void
build_base_obj_set_for_drs (VEC (data_reference_p, heap) *drs)
{
  int num_vertex = VEC_length (data_reference_p, drs);
  struct graph *g = new_graph (num_vertex);
  data_reference_p dr1, dr2;
  int i, j;
  int *queue;

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr1)
    for (j = i + 1; VEC_iterate (data_reference_p, drs, j, dr2); j++)
      if (dr_same_base_object_p (dr1, dr2))
	{
	  add_edge (g, i, j);
	  add_edge (g, j, i);
	}

  queue = XNEWVEC (int, num_vertex);
  for (i = 0; i < num_vertex; i++)
    queue[i] = i;

  graphds_dfs (g, queue, num_vertex, NULL, true, NULL);

  for (i = 0; i < g->n_vertices; i++)
    {
      data_reference_p dr = VEC_index (data_reference_p, drs, i);
      base_alias_pair *bap;

      gcc_assert (dr->aux);
      bap = (base_alias_pair *)(dr->aux);

      bap->base_obj_set = g->vertices[i].component + 1;
    }

  free (queue);
  free_graph (g);
}

/* Build the data references for PBB.  */

static void
build_pbb_drs (poly_bb_p pbb)
{
  int j;
  data_reference_p dr;
  VEC (data_reference_p, heap) *gbb_drs = GBB_DATA_REFS (PBB_BLACK_BOX (pbb));

  FOR_EACH_VEC_ELT (data_reference_p, gbb_drs, j, dr)
    build_poly_dr (dr, pbb);
}

/* Dump to file the alias graphs for the data references in DRS.  */

static void
dump_alias_graphs (VEC (data_reference_p, heap) *drs)
{
  char comment[100];
  FILE *file_dimacs, *file_ecc, *file_dot;

  file_dimacs = fopen ("/tmp/dr_alias_graph_dimacs", "ab");
  if (file_dimacs)
    {
      snprintf (comment, sizeof (comment), "%s %s", main_input_filename,
		current_function_name ());
      write_alias_graph_to_ascii_dimacs (file_dimacs, comment, drs);
      fclose (file_dimacs);
    }

  file_ecc = fopen ("/tmp/dr_alias_graph_ecc", "ab");
  if (file_ecc)
    {
      snprintf (comment, sizeof (comment), "%s %s", main_input_filename,
		current_function_name ());
      write_alias_graph_to_ascii_ecc (file_ecc, comment, drs);
      fclose (file_ecc);
    }

  file_dot = fopen ("/tmp/dr_alias_graph_dot", "ab");
  if (file_dot)
    {
      snprintf (comment, sizeof (comment), "%s %s", main_input_filename,
		current_function_name ());
      write_alias_graph_to_ascii_dot (file_dot, comment, drs);
      fclose (file_dot);
    }
}

/* Build data references in SCOP.  */

static void
build_scop_drs (scop_p scop)
{
  int i, j;
  poly_bb_p pbb;
  data_reference_p dr;
  VEC (data_reference_p, heap) *drs = VEC_alloc (data_reference_p, heap, 3);

  /* Remove all the PBBs that do not have data references: these basic
     blocks are not handled in the polyhedral representation.  */
  for (i = 0; VEC_iterate (poly_bb_p, SCOP_BBS (scop), i, pbb); i++)
    if (VEC_empty (data_reference_p, GBB_DATA_REFS (PBB_BLACK_BOX (pbb))))
      {
	free_gimple_bb (PBB_BLACK_BOX (pbb));
	VEC_ordered_remove (poly_bb_p, SCOP_BBS (scop), i);
	i--;
      }

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    for (j = 0; VEC_iterate (data_reference_p,
			     GBB_DATA_REFS (PBB_BLACK_BOX (pbb)), j, dr); j++)
      VEC_safe_push (data_reference_p, heap, drs, dr);

  FOR_EACH_VEC_ELT (data_reference_p, drs, i, dr)
    dr->aux = XNEW (base_alias_pair);

  if (!build_alias_set_optimal_p (drs))
    {
      /* TODO: Add support when building alias set is not optimal.  */
      ;
    }

  build_base_obj_set_for_drs (drs);

  /* When debugging, enable the following code.  This cannot be used
     in production compilers.  */
  if (0)
    dump_alias_graphs (drs);

  VEC_free (data_reference_p, heap, drs);

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    build_pbb_drs (pbb);
}

/* Return a gsi at the position of the phi node STMT.  */

static gimple_stmt_iterator
gsi_for_phi_node (gimple stmt)
{
  gimple_stmt_iterator psi;
  basic_block bb = gimple_bb (stmt);

  for (psi = gsi_start_phis (bb); !gsi_end_p (psi); gsi_next (&psi))
    if (stmt == gsi_stmt (psi))
      return psi;

  gcc_unreachable ();
  return psi;
}

/* Analyze all the data references of STMTS and add them to the
   GBB_DATA_REFS vector of BB.  */

static void
analyze_drs_in_stmts (scop_p scop, basic_block bb, VEC (gimple, heap) *stmts)
{
  loop_p nest;
  gimple_bb_p gbb;
  gimple stmt;
  int i;
  sese region = SCOP_REGION (scop);

  if (!bb_in_sese_p (bb, region))
    return;

  nest = outermost_loop_in_sese_1 (region, bb);
  gbb = gbb_from_bb (bb);

  FOR_EACH_VEC_ELT (gimple, stmts, i, stmt)
    {
      loop_p loop;

      if (is_gimple_debug (stmt))
	continue;

      loop = loop_containing_stmt (stmt);
      if (!loop_in_sese_p (loop, region))
	loop = nest;

      graphite_find_data_references_in_stmt (nest, loop, stmt,
					     &GBB_DATA_REFS (gbb));
    }
}

/* Insert STMT at the end of the STMTS sequence and then insert the
   statements from STMTS at INSERT_GSI and call analyze_drs_in_stmts
   on STMTS.  */

static void
insert_stmts (scop_p scop, gimple stmt, gimple_seq stmts,
	      gimple_stmt_iterator insert_gsi)
{
  gimple_stmt_iterator gsi;
  VEC (gimple, heap) *x = VEC_alloc (gimple, heap, 3);

  if (!stmts)
    stmts = gimple_seq_alloc ();

  gsi = gsi_last (stmts);
  gsi_insert_after (&gsi, stmt, GSI_NEW_STMT);
  for (gsi = gsi_start (stmts); !gsi_end_p (gsi); gsi_next (&gsi))
    VEC_safe_push (gimple, heap, x, gsi_stmt (gsi));

  gsi_insert_seq_before (&insert_gsi, stmts, GSI_SAME_STMT);
  analyze_drs_in_stmts (scop, gsi_bb (insert_gsi), x);
  VEC_free (gimple, heap, x);
}

/* Insert the assignment "RES := EXPR" just after AFTER_STMT.  */

static void
insert_out_of_ssa_copy (scop_p scop, tree res, tree expr, gimple after_stmt)
{
  gimple_seq stmts;
  gimple_stmt_iterator si;
  gimple_stmt_iterator gsi;
  tree var = force_gimple_operand (expr, &stmts, true, NULL_TREE);
  gimple stmt = gimple_build_assign (res, var);
  VEC (gimple, heap) *x = VEC_alloc (gimple, heap, 3);

  if (!stmts)
    stmts = gimple_seq_alloc ();
  si = gsi_last (stmts);
  gsi_insert_after (&si, stmt, GSI_NEW_STMT);
  for (gsi = gsi_start (stmts); !gsi_end_p (gsi); gsi_next (&gsi))
    VEC_safe_push (gimple, heap, x, gsi_stmt (gsi));

  if (gimple_code (after_stmt) == GIMPLE_PHI)
    {
      gsi = gsi_after_labels (gimple_bb (after_stmt));
      gsi_insert_seq_before (&gsi, stmts, GSI_NEW_STMT);
    }
  else
    {
      gsi = gsi_for_stmt (after_stmt);
      gsi_insert_seq_after (&gsi, stmts, GSI_NEW_STMT);
    }

  analyze_drs_in_stmts (scop, gimple_bb (after_stmt), x);
  VEC_free (gimple, heap, x);
}

/* Creates a poly_bb_p for basic_block BB from the existing PBB.  */

static void
new_pbb_from_pbb (scop_p scop, poly_bb_p pbb, basic_block bb)
{
  VEC (data_reference_p, heap) *drs = VEC_alloc (data_reference_p, heap, 3);
  gimple_bb_p gbb = PBB_BLACK_BOX (pbb);
  gimple_bb_p gbb1 = new_gimple_bb (bb, drs);
  poly_bb_p pbb1 = new_poly_bb (scop, gbb1);
  int index, n = VEC_length (poly_bb_p, SCOP_BBS (scop));

  /* The INDEX of PBB in SCOP_BBS.  */
  for (index = 0; index < n; index++)
    if (VEC_index (poly_bb_p, SCOP_BBS (scop), index) == pbb)
      break;

  if (PBB_DOMAIN (pbb))
    ppl_new_Pointset_Powerset_C_Polyhedron_from_Pointset_Powerset_C_Polyhedron
      (&PBB_DOMAIN (pbb1), PBB_DOMAIN (pbb));

  GBB_PBB (gbb1) = pbb1;
  GBB_CONDITIONS (gbb1) = VEC_copy (gimple, heap, GBB_CONDITIONS (gbb));
  GBB_CONDITION_CASES (gbb1) = VEC_copy (gimple, heap, GBB_CONDITION_CASES (gbb));
  VEC_safe_insert (poly_bb_p, heap, SCOP_BBS (scop), index + 1, pbb1);
}

/* Insert on edge E the assignment "RES := EXPR".  */

static void
insert_out_of_ssa_copy_on_edge (scop_p scop, edge e, tree res, tree expr)
{
  gimple_stmt_iterator gsi;
  gimple_seq stmts;
  tree var = force_gimple_operand (expr, &stmts, true, NULL_TREE);
  gimple stmt = gimple_build_assign (res, var);
  basic_block bb;
  VEC (gimple, heap) *x = VEC_alloc (gimple, heap, 3);

  if (!stmts)
    stmts = gimple_seq_alloc ();

  gsi = gsi_last (stmts);
  gsi_insert_after (&gsi, stmt, GSI_NEW_STMT);
  for (gsi = gsi_start (stmts); !gsi_end_p (gsi); gsi_next (&gsi))
    VEC_safe_push (gimple, heap, x, gsi_stmt (gsi));

  gsi_insert_seq_on_edge (e, stmts);
  gsi_commit_edge_inserts ();
  bb = gimple_bb (stmt);

  if (!bb_in_sese_p (bb, SCOP_REGION (scop)))
    return;

  if (!gbb_from_bb (bb))
    new_pbb_from_pbb (scop, pbb_from_bb (e->src), bb);

  analyze_drs_in_stmts (scop, bb, x);
  VEC_free (gimple, heap, x);
}

/* Creates a zero dimension array of the same type as VAR.  */

static tree
create_zero_dim_array (tree var, const char *base_name)
{
  tree index_type = build_index_type (integer_zero_node);
  tree elt_type = TREE_TYPE (var);
  tree array_type = build_array_type (elt_type, index_type);
  tree base = create_tmp_var (array_type, base_name);

  add_referenced_var (base);

  return build4 (ARRAY_REF, elt_type, base, integer_zero_node, NULL_TREE,
		 NULL_TREE);
}

/* Returns true when PHI is a loop close phi node.  */

static bool
scalar_close_phi_node_p (gimple phi)
{
  if (gimple_code (phi) != GIMPLE_PHI
      || !is_gimple_reg (gimple_phi_result (phi)))
    return false;

  /* Note that loop close phi nodes should have a single argument
     because we translated the representation into a canonical form
     before Graphite: see canonicalize_loop_closed_ssa_form.  */
  return (gimple_phi_num_args (phi) == 1);
}

/* For a definition DEF in REGION, propagates the expression EXPR in
   all the uses of DEF outside REGION.  */

static void
propagate_expr_outside_region (tree def, tree expr, sese region)
{
  imm_use_iterator imm_iter;
  gimple use_stmt;
  gimple_seq stmts;
  bool replaced_once = false;

  gcc_assert (TREE_CODE (def) == SSA_NAME);

  expr = force_gimple_operand (unshare_expr (expr), &stmts, true,
			       NULL_TREE);

  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, def)
    if (!is_gimple_debug (use_stmt)
	&& !bb_in_sese_p (gimple_bb (use_stmt), region))
      {
	ssa_op_iter iter;
	use_operand_p use_p;

	FOR_EACH_PHI_OR_STMT_USE (use_p, use_stmt, iter, SSA_OP_ALL_USES)
	  if (operand_equal_p (def, USE_FROM_PTR (use_p), 0)
	      && (replaced_once = true))
	    replace_exp (use_p, expr);

	update_stmt (use_stmt);
      }

  if (replaced_once)
    {
      gsi_insert_seq_on_edge (SESE_ENTRY (region), stmts);
      gsi_commit_edge_inserts ();
    }
}

/* Rewrite out of SSA the reduction phi node at PSI by creating a zero
   dimension array for it.  */

static void
rewrite_close_phi_out_of_ssa (scop_p scop, gimple_stmt_iterator *psi)
{
  sese region = SCOP_REGION (scop);
  gimple phi = gsi_stmt (*psi);
  tree res = gimple_phi_result (phi);
  tree var = SSA_NAME_VAR (res);
  basic_block bb = gimple_bb (phi);
  gimple_stmt_iterator gsi = gsi_after_labels (bb);
  tree arg = gimple_phi_arg_def (phi, 0);
  gimple stmt;

  /* Note that loop close phi nodes should have a single argument
     because we translated the representation into a canonical form
     before Graphite: see canonicalize_loop_closed_ssa_form.  */
  gcc_assert (gimple_phi_num_args (phi) == 1);

  /* The phi node can be a non close phi node, when its argument is
     invariant, or a default definition.  */
  if (is_gimple_min_invariant (arg)
      || SSA_NAME_IS_DEFAULT_DEF (arg))
    {
      propagate_expr_outside_region (res, arg, region);
      gsi_next (psi);
      return;
    }

  else if (gimple_bb (SSA_NAME_DEF_STMT (arg))->loop_father == bb->loop_father)
    {
      propagate_expr_outside_region (res, arg, region);
      stmt = gimple_build_assign (res, arg);
      remove_phi_node (psi, false);
      gsi_insert_before (&gsi, stmt, GSI_NEW_STMT);
      SSA_NAME_DEF_STMT (res) = stmt;
      return;
    }

  /* If res is scev analyzable and is not a scalar value, it is safe
     to ignore the close phi node: it will be code generated in the
     out of Graphite pass.  */
  else if (scev_analyzable_p (res, region))
    {
      loop_p loop = loop_containing_stmt (SSA_NAME_DEF_STMT (res));
      tree scev;

      if (!loop_in_sese_p (loop, region))
	{
	  loop = loop_containing_stmt (SSA_NAME_DEF_STMT (arg));
	  scev = scalar_evolution_in_region (region, loop, arg);
	  scev = compute_overall_effect_of_inner_loop (loop, scev);
	}
      else
	scev = scalar_evolution_in_region (region, loop, res);

      if (tree_does_not_contain_chrecs (scev))
	propagate_expr_outside_region (res, scev, region);

      gsi_next (psi);
      return;
    }
  else
    {
      tree zero_dim_array = create_zero_dim_array (var, "Close_Phi");

      stmt = gimple_build_assign (res, zero_dim_array);

      if (TREE_CODE (arg) == SSA_NAME)
	insert_out_of_ssa_copy (scop, zero_dim_array, arg,
				SSA_NAME_DEF_STMT (arg));
      else
	insert_out_of_ssa_copy_on_edge (scop, single_pred_edge (bb),
					zero_dim_array, arg);
    }

  remove_phi_node (psi, false);
  SSA_NAME_DEF_STMT (res) = stmt;

  insert_stmts (scop, stmt, NULL, gsi_after_labels (bb));
}

/* Rewrite out of SSA the reduction phi node at PSI by creating a zero
   dimension array for it.  */

static void
rewrite_phi_out_of_ssa (scop_p scop, gimple_stmt_iterator *psi)
{
  size_t i;
  gimple phi = gsi_stmt (*psi);
  basic_block bb = gimple_bb (phi);
  tree res = gimple_phi_result (phi);
  tree var = SSA_NAME_VAR (res);
  tree zero_dim_array = create_zero_dim_array (var, "phi_out_of_ssa");
  gimple stmt;
  gimple_seq stmts;

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    {
      tree arg = gimple_phi_arg_def (phi, i);
      edge e = gimple_phi_arg_edge (phi, i);

      /* Avoid the insertion of code in the loop latch to please the
	 pattern matching of the vectorizer.  */
      if (TREE_CODE (arg) == SSA_NAME
	  && e->src == bb->loop_father->latch)
	insert_out_of_ssa_copy (scop, zero_dim_array, arg,
				SSA_NAME_DEF_STMT (arg));
      else
	insert_out_of_ssa_copy_on_edge (scop, e, zero_dim_array, arg);
    }

  var = force_gimple_operand (zero_dim_array, &stmts, true, NULL_TREE);

  stmt = gimple_build_assign (res, var);
  remove_phi_node (psi, false);
  SSA_NAME_DEF_STMT (res) = stmt;

  insert_stmts (scop, stmt, stmts, gsi_after_labels (bb));
}

/* Rewrite the degenerate phi node at position PSI from the degenerate
   form "x = phi (y, y, ..., y)" to "x = y".  */

static void
rewrite_degenerate_phi (gimple_stmt_iterator *psi)
{
  tree rhs;
  gimple stmt;
  gimple_stmt_iterator gsi;
  gimple phi = gsi_stmt (*psi);
  tree res = gimple_phi_result (phi);
  basic_block bb;

  bb = gimple_bb (phi);
  rhs = degenerate_phi_result (phi);
  gcc_assert (rhs);

  stmt = gimple_build_assign (res, rhs);
  remove_phi_node (psi, false);
  SSA_NAME_DEF_STMT (res) = stmt;

  gsi = gsi_after_labels (bb);
  gsi_insert_before (&gsi, stmt, GSI_NEW_STMT);
}

/* Rewrite out of SSA all the reduction phi nodes of SCOP.  */

static void
rewrite_reductions_out_of_ssa (scop_p scop)
{
  basic_block bb;
  gimple_stmt_iterator psi;
  sese region = SCOP_REGION (scop);

  FOR_EACH_BB (bb)
    if (bb_in_sese_p (bb, region))
      for (psi = gsi_start_phis (bb); !gsi_end_p (psi);)
	{
	  gimple phi = gsi_stmt (psi);

	  if (!is_gimple_reg (gimple_phi_result (phi)))
	    {
	      gsi_next (&psi);
	      continue;
	    }

	  if (gimple_phi_num_args (phi) > 1
	      && degenerate_phi_result (phi))
	    rewrite_degenerate_phi (&psi);

	  else if (scalar_close_phi_node_p (phi))
	    rewrite_close_phi_out_of_ssa (scop, &psi);

	  else if (reduction_phi_p (region, &psi))
	    rewrite_phi_out_of_ssa (scop, &psi);
	}

  update_ssa (TODO_update_ssa);
#ifdef ENABLE_CHECKING
  verify_loop_closed_ssa (true);
#endif
}

/* Rewrite the scalar dependence of DEF used in USE_STMT with a memory
   read from ZERO_DIM_ARRAY.  */

static void
rewrite_cross_bb_scalar_dependence (scop_p scop, tree zero_dim_array,
				    tree def, gimple use_stmt)
{
  tree var = SSA_NAME_VAR (def);
  gimple name_stmt = gimple_build_assign (var, zero_dim_array);
  tree name = make_ssa_name (var, name_stmt);
  ssa_op_iter iter;
  use_operand_p use_p;

  gcc_assert (gimple_code (use_stmt) != GIMPLE_PHI);

  gimple_assign_set_lhs (name_stmt, name);
  insert_stmts (scop, name_stmt, NULL, gsi_for_stmt (use_stmt));

  FOR_EACH_SSA_USE_OPERAND (use_p, use_stmt, iter, SSA_OP_ALL_USES)
    if (operand_equal_p (def, USE_FROM_PTR (use_p), 0))
      replace_exp (use_p, name);

  update_stmt (use_stmt);
}

/* For every definition DEF in the SCOP that is used outside the scop,
   insert a closing-scop definition in the basic block just after this
   SCOP.  */

static void
handle_scalar_deps_crossing_scop_limits (scop_p scop, tree def, gimple stmt)
{
  tree var = create_tmp_reg (TREE_TYPE (def), NULL);
  tree new_name = make_ssa_name (var, stmt);
  bool needs_copy = false;
  use_operand_p use_p;
  imm_use_iterator imm_iter;
  gimple use_stmt;
  sese region = SCOP_REGION (scop);

  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, def)
    {
      if (!bb_in_sese_p (gimple_bb (use_stmt), region))
	{
	  FOR_EACH_IMM_USE_ON_STMT (use_p, imm_iter)
	    {
	      SET_USE (use_p, new_name);
	    }
	  update_stmt (use_stmt);
	  needs_copy = true;
	}
    }

  /* Insert in the empty BB just after the scop a use of DEF such
     that the rewrite of cross_bb_scalar_dependences won't insert
     arrays everywhere else.  */
  if (needs_copy)
    {
      gimple assign = gimple_build_assign (new_name, def);
      gimple_stmt_iterator psi = gsi_after_labels (SESE_EXIT (region)->dest);

      add_referenced_var (var);
      SSA_NAME_DEF_STMT (new_name) = assign;
      update_stmt (assign);
      gsi_insert_before (&psi, assign, GSI_SAME_STMT);
    }
}

/* Rewrite the scalar dependences crossing the boundary of the BB
   containing STMT with an array.  Return true when something has been
   changed.  */

static bool
rewrite_cross_bb_scalar_deps (scop_p scop, gimple_stmt_iterator *gsi)
{
  sese region = SCOP_REGION (scop);
  gimple stmt = gsi_stmt (*gsi);
  imm_use_iterator imm_iter;
  tree def;
  basic_block def_bb;
  tree zero_dim_array = NULL_TREE;
  gimple use_stmt;
  bool res = false;

  switch (gimple_code (stmt))
    {
    case GIMPLE_ASSIGN:
      def = gimple_assign_lhs (stmt);
      break;

    case GIMPLE_CALL:
      def = gimple_call_lhs (stmt);
      break;

    default:
      return false;
    }

  if (!def
      || !is_gimple_reg (def))
    return false;

  if (scev_analyzable_p (def, region))
    {
      loop_p loop = loop_containing_stmt (SSA_NAME_DEF_STMT (def));
      tree scev = scalar_evolution_in_region (region, loop, def);

      if (tree_contains_chrecs (scev, NULL))
	return false;

      propagate_expr_outside_region (def, scev, region);
      return true;
    }

  def_bb = gimple_bb (stmt);

  handle_scalar_deps_crossing_scop_limits (scop, def, stmt);

  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, def)
    if (gimple_code (use_stmt) == GIMPLE_PHI
	&& (res = true))
      {
	gimple_stmt_iterator psi = gsi_for_stmt (use_stmt);

	if (scalar_close_phi_node_p (gsi_stmt (psi)))
	  rewrite_close_phi_out_of_ssa (scop, &psi);
	else
	  rewrite_phi_out_of_ssa (scop, &psi);
      }

  FOR_EACH_IMM_USE_STMT (use_stmt, imm_iter, def)
    if (gimple_code (use_stmt) != GIMPLE_PHI
	&& def_bb != gimple_bb (use_stmt)
	&& !is_gimple_debug (use_stmt)
	&& (res = true))
      {
	if (!zero_dim_array)
	  {
	    zero_dim_array = create_zero_dim_array
	      (SSA_NAME_VAR (def), "Cross_BB_scalar_dependence");
	    insert_out_of_ssa_copy (scop, zero_dim_array, def,
				    SSA_NAME_DEF_STMT (def));
	    gsi_next (gsi);
	  }

	rewrite_cross_bb_scalar_dependence (scop, zero_dim_array,
					    def, use_stmt);
      }

  return res;
}

/* Rewrite out of SSA all the reduction phi nodes of SCOP.  */

static void
rewrite_cross_bb_scalar_deps_out_of_ssa (scop_p scop)
{
  basic_block bb;
  gimple_stmt_iterator psi;
  sese region = SCOP_REGION (scop);
  bool changed = false;

  /* Create an extra empty BB after the scop.  */
  split_edge (SESE_EXIT (region));

  FOR_EACH_BB (bb)
    if (bb_in_sese_p (bb, region))
      for (psi = gsi_start_bb (bb); !gsi_end_p (psi); gsi_next (&psi))
	changed |= rewrite_cross_bb_scalar_deps (scop, &psi);

  if (changed)
    {
      scev_reset_htab ();
      update_ssa (TODO_update_ssa);
#ifdef ENABLE_CHECKING
      verify_loop_closed_ssa (true);
#endif
    }
}

/* Returns the number of pbbs that are in loops contained in SCOP.  */

static int
nb_pbbs_in_loops (scop_p scop)
{
  int i;
  poly_bb_p pbb;
  int res = 0;

  FOR_EACH_VEC_ELT (poly_bb_p, SCOP_BBS (scop), i, pbb)
    if (loop_in_sese_p (gbb_loop (PBB_BLACK_BOX (pbb)), SCOP_REGION (scop)))
      res++;

  return res;
}

/* Return the number of data references in BB that write in
   memory.  */

static int
nb_data_writes_in_bb (basic_block bb)
{
  int res = 0;
  gimple_stmt_iterator gsi;

  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    if (gimple_vdef (gsi_stmt (gsi)))
      res++;

  return res;
}

/* Splits at STMT the basic block BB represented as PBB in the
   polyhedral form.  */

static edge
split_pbb (scop_p scop, poly_bb_p pbb, basic_block bb, gimple stmt)
{
  edge e1 = split_block (bb, stmt);
  new_pbb_from_pbb (scop, pbb, e1->dest);
  return e1;
}

/* Splits STMT out of its current BB.  This is done for reduction
   statements for which we want to ignore data dependences.  */

static basic_block
split_reduction_stmt (scop_p scop, gimple stmt)
{
  basic_block bb = gimple_bb (stmt);
  poly_bb_p pbb = pbb_from_bb (bb);
  gimple_bb_p gbb = gbb_from_bb (bb);
  edge e1;
  int i;
  data_reference_p dr;

  /* Do not split basic blocks with no writes to memory: the reduction
     will be the only write to memory.  */
  if (nb_data_writes_in_bb (bb) == 0
      /* Or if we have already marked BB as a reduction.  */
      || PBB_IS_REDUCTION (pbb_from_bb (bb)))
    return bb;

  e1 = split_pbb (scop, pbb, bb, stmt);

  /* Split once more only when the reduction stmt is not the only one
     left in the original BB.  */
  if (!gsi_one_before_end_p (gsi_start_nondebug_bb (bb)))
    {
      gimple_stmt_iterator gsi = gsi_last_bb (bb);
      gsi_prev (&gsi);
      e1 = split_pbb (scop, pbb, bb, gsi_stmt (gsi));
    }

  /* A part of the data references will end in a different basic block
     after the split: move the DRs from the original GBB to the newly
     created GBB1.  */
  FOR_EACH_VEC_ELT (data_reference_p, GBB_DATA_REFS (gbb), i, dr)
    {
      basic_block bb1 = gimple_bb (DR_STMT (dr));

      if (bb1 != bb)
	{
	  gimple_bb_p gbb1 = gbb_from_bb (bb1);
	  VEC_safe_push (data_reference_p, heap, GBB_DATA_REFS (gbb1), dr);
	  VEC_ordered_remove (data_reference_p, GBB_DATA_REFS (gbb), i);
	  i--;
	}
    }

  return e1->dest;
}

/* Return true when stmt is a reduction operation.  */

static inline bool
is_reduction_operation_p (gimple stmt)
{
  enum tree_code code;

  gcc_assert (is_gimple_assign (stmt));
  code = gimple_assign_rhs_code (stmt);

  return flag_associative_math
    && commutative_tree_code (code)
    && associative_tree_code (code);
}

/* Returns true when PHI contains an argument ARG.  */

static bool
phi_contains_arg (gimple phi, tree arg)
{
  size_t i;

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    if (operand_equal_p (arg, gimple_phi_arg_def (phi, i), 0))
      return true;

  return false;
}

/* Return a loop phi node that corresponds to a reduction containing LHS.  */

static gimple
follow_ssa_with_commutative_ops (tree arg, tree lhs)
{
  gimple stmt;

  if (TREE_CODE (arg) != SSA_NAME)
    return NULL;

  stmt = SSA_NAME_DEF_STMT (arg);

  if (gimple_code (stmt) == GIMPLE_NOP
      || gimple_code (stmt) == GIMPLE_CALL)
    return NULL;

  if (gimple_code (stmt) == GIMPLE_PHI)
    {
      if (phi_contains_arg (stmt, lhs))
	return stmt;
      return NULL;
    }

  if (!is_gimple_assign (stmt))
    return NULL;

  if (gimple_num_ops (stmt) == 2)
    return follow_ssa_with_commutative_ops (gimple_assign_rhs1 (stmt), lhs);

  if (is_reduction_operation_p (stmt))
    {
      gimple res = follow_ssa_with_commutative_ops (gimple_assign_rhs1 (stmt), lhs);

      return res ? res :
	follow_ssa_with_commutative_ops (gimple_assign_rhs2 (stmt), lhs);
    }

  return NULL;
}

/* Detect commutative and associative scalar reductions starting at
   the STMT.  Return the phi node of the reduction cycle, or NULL.  */

static gimple
detect_commutative_reduction_arg (tree lhs, gimple stmt, tree arg,
				  VEC (gimple, heap) **in,
				  VEC (gimple, heap) **out)
{
  gimple phi = follow_ssa_with_commutative_ops (arg, lhs);

  if (!phi)
    return NULL;

  VEC_safe_push (gimple, heap, *in, stmt);
  VEC_safe_push (gimple, heap, *out, stmt);
  return phi;
}

/* Detect commutative and associative scalar reductions starting at
   STMT.  Return the phi node of the reduction cycle, or NULL.  */

static gimple
detect_commutative_reduction_assign (gimple stmt, VEC (gimple, heap) **in,
				     VEC (gimple, heap) **out)
{
  tree lhs = gimple_assign_lhs (stmt);

  if (gimple_num_ops (stmt) == 2)
    return detect_commutative_reduction_arg (lhs, stmt,
					     gimple_assign_rhs1 (stmt),
					     in, out);

  if (is_reduction_operation_p (stmt))
    {
      gimple res = detect_commutative_reduction_arg (lhs, stmt,
						     gimple_assign_rhs1 (stmt),
						     in, out);
      return res ? res
	: detect_commutative_reduction_arg (lhs, stmt,
					    gimple_assign_rhs2 (stmt),
					    in, out);
    }

  return NULL;
}

/* Return a loop phi node that corresponds to a reduction containing LHS.  */

static gimple
follow_inital_value_to_phi (tree arg, tree lhs)
{
  gimple stmt;

  if (!arg || TREE_CODE (arg) != SSA_NAME)
    return NULL;

  stmt = SSA_NAME_DEF_STMT (arg);

  if (gimple_code (stmt) == GIMPLE_PHI
      && phi_contains_arg (stmt, lhs))
    return stmt;

  return NULL;
}


/* Return the argument of the loop PHI that is the inital value coming
   from outside the loop.  */

static edge
edge_initial_value_for_loop_phi (gimple phi)
{
  size_t i;

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    {
      edge e = gimple_phi_arg_edge (phi, i);

      if (loop_depth (e->src->loop_father)
	  < loop_depth (e->dest->loop_father))
	return e;
    }

  return NULL;
}

/* Return the argument of the loop PHI that is the inital value coming
   from outside the loop.  */

static tree
initial_value_for_loop_phi (gimple phi)
{
  size_t i;

  for (i = 0; i < gimple_phi_num_args (phi); i++)
    {
      edge e = gimple_phi_arg_edge (phi, i);

      if (loop_depth (e->src->loop_father)
	  < loop_depth (e->dest->loop_father))
	return gimple_phi_arg_def (phi, i);
    }

  return NULL_TREE;
}

/* Returns true when DEF is used outside the reduction cycle of
   LOOP_PHI.  */

static bool
used_outside_reduction (tree def, gimple loop_phi)
{
  use_operand_p use_p;
  imm_use_iterator imm_iter;
  loop_p loop = loop_containing_stmt (loop_phi);

  /* In LOOP, DEF should be used only in LOOP_PHI.  */
  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, def)
    {
      gimple stmt = USE_STMT (use_p);

      if (stmt != loop_phi
	  && !is_gimple_debug (stmt)
	  && flow_bb_inside_loop_p (loop, gimple_bb (stmt)))
	return true;
    }

  return false;
}

/* Detect commutative and associative scalar reductions belonging to
   the SCOP starting at the loop closed phi node STMT.  Return the phi
   node of the reduction cycle, or NULL.  */

static gimple
detect_commutative_reduction (scop_p scop, gimple stmt, VEC (gimple, heap) **in,
			      VEC (gimple, heap) **out)
{
  if (scalar_close_phi_node_p (stmt))
    {
      gimple def, loop_phi, phi, close_phi = stmt;
      tree init, lhs, arg = gimple_phi_arg_def (close_phi, 0);

      if (TREE_CODE (arg) != SSA_NAME)
	return NULL;

      /* Note that loop close phi nodes should have a single argument
	 because we translated the representation into a canonical form
	 before Graphite: see canonicalize_loop_closed_ssa_form.  */
      gcc_assert (gimple_phi_num_args (close_phi) == 1);

      def = SSA_NAME_DEF_STMT (arg);
      if (!stmt_in_sese_p (def, SCOP_REGION (scop))
	  || !(loop_phi = detect_commutative_reduction (scop, def, in, out)))
	return NULL;

      lhs = gimple_phi_result (close_phi);
      init = initial_value_for_loop_phi (loop_phi);
      phi = follow_inital_value_to_phi (init, lhs);

      if (phi && (used_outside_reduction (lhs, phi)
		  || !has_single_use (gimple_phi_result (phi))))
	return NULL;

      VEC_safe_push (gimple, heap, *in, loop_phi);
      VEC_safe_push (gimple, heap, *out, close_phi);
      return phi;
    }

  if (gimple_code (stmt) == GIMPLE_ASSIGN)
    return detect_commutative_reduction_assign (stmt, in, out);

  return NULL;
}

/* Translate the scalar reduction statement STMT to an array RED
   knowing that its recursive phi node is LOOP_PHI.  */

static void
translate_scalar_reduction_to_array_for_stmt (scop_p scop, tree red,
					      gimple stmt, gimple loop_phi)
{
  tree res = gimple_phi_result (loop_phi);
  gimple assign = gimple_build_assign (res, unshare_expr (red));
  gimple_stmt_iterator gsi;

  insert_stmts (scop, assign, NULL, gsi_after_labels (gimple_bb (loop_phi)));

  assign = gimple_build_assign (unshare_expr (red), gimple_assign_lhs (stmt));
  gsi = gsi_for_stmt (stmt);
  gsi_next (&gsi);
  insert_stmts (scop, assign, NULL, gsi);
}

/* Removes the PHI node and resets all the debug stmts that are using
   the PHI_RESULT.  */

static void
remove_phi (gimple phi)
{
  imm_use_iterator imm_iter;
  tree def;
  use_operand_p use_p;
  gimple_stmt_iterator gsi;
  VEC (gimple, heap) *update = VEC_alloc (gimple, heap, 3);
  unsigned int i;
  gimple stmt;

  def = PHI_RESULT (phi);
  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, def)
    {
      stmt = USE_STMT (use_p);

      if (is_gimple_debug (stmt))
	{
	  gimple_debug_bind_reset_value (stmt);
	  VEC_safe_push (gimple, heap, update, stmt);
	}
    }

  FOR_EACH_VEC_ELT (gimple, update, i, stmt)
    update_stmt (stmt);

  VEC_free (gimple, heap, update);

  gsi = gsi_for_phi_node (phi);
  remove_phi_node (&gsi, false);
}

/* Helper function for for_each_index.  For each INDEX of the data
   reference REF, returns true when its indices are valid in the loop
   nest LOOP passed in as DATA.  */

static bool
dr_indices_valid_in_loop (tree ref ATTRIBUTE_UNUSED, tree *index, void *data)
{
  loop_p loop;
  basic_block header, def_bb;
  gimple stmt;

  if (TREE_CODE (*index) != SSA_NAME)
    return true;

  loop = *((loop_p *) data);
  header = loop->header;
  stmt = SSA_NAME_DEF_STMT (*index);

  if (!stmt)
    return true;

  def_bb = gimple_bb (stmt);

  if (!def_bb)
    return true;

  return dominated_by_p (CDI_DOMINATORS, header, def_bb);
}

/* When the result of a CLOSE_PHI is written to a memory location,
   return a pointer to that memory reference, otherwise return
   NULL_TREE.  */

static tree
close_phi_written_to_memory (gimple close_phi)
{
  imm_use_iterator imm_iter;
  use_operand_p use_p;
  gimple stmt;
  tree res, def = gimple_phi_result (close_phi);

  FOR_EACH_IMM_USE_FAST (use_p, imm_iter, def)
    if ((stmt = USE_STMT (use_p))
	&& gimple_code (stmt) == GIMPLE_ASSIGN
	&& (res = gimple_assign_lhs (stmt)))
      {
	switch (TREE_CODE (res))
	  {
	  case VAR_DECL:
	  case PARM_DECL:
	  case RESULT_DECL:
	    return res;

	  case ARRAY_REF:
	  case MEM_REF:
	    {
	      tree arg = gimple_phi_arg_def (close_phi, 0);
	      loop_p nest = loop_containing_stmt (SSA_NAME_DEF_STMT (arg));

	      /* FIXME: this restriction is for id-{24,25}.f and
		 could be handled by duplicating the computation of
		 array indices before the loop of the close_phi.  */
	      if (for_each_index (&res, dr_indices_valid_in_loop, &nest))
		return res;
	    }
	    /* Fallthru.  */

	  default:
	    continue;
	  }
      }
  return NULL_TREE;
}

/* Rewrite out of SSA the reduction described by the loop phi nodes
   IN, and the close phi nodes OUT.  IN and OUT are structured by loop
   levels like this:

   IN: stmt, loop_n, ..., loop_0
   OUT: stmt, close_n, ..., close_0

   the first element is the reduction statement, and the next elements
   are the loop and close phi nodes of each of the outer loops.  */

static void
translate_scalar_reduction_to_array (scop_p scop,
				     VEC (gimple, heap) *in,
				     VEC (gimple, heap) *out)
{
  gimple loop_phi;
  unsigned int i = VEC_length (gimple, out) - 1;
  tree red = close_phi_written_to_memory (VEC_index (gimple, out, i));

  FOR_EACH_VEC_ELT (gimple, in, i, loop_phi)
    {
      gimple close_phi = VEC_index (gimple, out, i);

      if (i == 0)
	{
	  gimple stmt = loop_phi;
	  basic_block bb = split_reduction_stmt (scop, stmt);
	  poly_bb_p pbb = pbb_from_bb (bb);
	  PBB_IS_REDUCTION (pbb) = true;
	  gcc_assert (close_phi == loop_phi);

	  if (!red)
	    red = create_zero_dim_array
	      (gimple_assign_lhs (stmt), "Commutative_Associative_Reduction");

	  translate_scalar_reduction_to_array_for_stmt
	    (scop, red, stmt, VEC_index (gimple, in, 1));
	  continue;
	}

      if (i == VEC_length (gimple, in) - 1)
	{
	  insert_out_of_ssa_copy (scop, gimple_phi_result (close_phi),
				  unshare_expr (red), close_phi);
	  insert_out_of_ssa_copy_on_edge
	    (scop, edge_initial_value_for_loop_phi (loop_phi),
	     unshare_expr (red), initial_value_for_loop_phi (loop_phi));
	}

      remove_phi (loop_phi);
      remove_phi (close_phi);
    }
}

/* Rewrites out of SSA a commutative reduction at CLOSE_PHI.  Returns
   true when something has been changed.  */

static bool
rewrite_commutative_reductions_out_of_ssa_close_phi (scop_p scop,
						     gimple close_phi)
{
  bool res;
  VEC (gimple, heap) *in = VEC_alloc (gimple, heap, 10);
  VEC (gimple, heap) *out = VEC_alloc (gimple, heap, 10);

  detect_commutative_reduction (scop, close_phi, &in, &out);
  res = VEC_length (gimple, in) > 1;
  if (res)
    translate_scalar_reduction_to_array (scop, in, out);

  VEC_free (gimple, heap, in);
  VEC_free (gimple, heap, out);
  return res;
}

/* Rewrites all the commutative reductions from LOOP out of SSA.
   Returns true when something has been changed.  */

static bool
rewrite_commutative_reductions_out_of_ssa_loop (scop_p scop,
						loop_p loop)
{
  gimple_stmt_iterator gsi;
  edge exit = single_exit (loop);
  tree res;
  bool changed = false;

  if (!exit)
    return false;

  for (gsi = gsi_start_phis (exit->dest); !gsi_end_p (gsi); gsi_next (&gsi))
    if ((res = gimple_phi_result (gsi_stmt (gsi)))
	&& is_gimple_reg (res)
	&& !scev_analyzable_p (res, SCOP_REGION (scop)))
      changed |= rewrite_commutative_reductions_out_of_ssa_close_phi
	(scop, gsi_stmt (gsi));

  return changed;
}

/* Rewrites all the commutative reductions from SCOP out of SSA.  */

static void
rewrite_commutative_reductions_out_of_ssa (scop_p scop)
{
  loop_iterator li;
  loop_p loop;
  bool changed = false;
  sese region = SCOP_REGION (scop);

  FOR_EACH_LOOP (li, loop, 0)
    if (loop_in_sese_p (loop, region))
      changed |= rewrite_commutative_reductions_out_of_ssa_loop (scop, loop);

  if (changed)
    {
      scev_reset_htab ();
      gsi_commit_edge_inserts ();
      update_ssa (TODO_update_ssa);
#ifdef ENABLE_CHECKING
      verify_loop_closed_ssa (true);
#endif
    }
}

/* Can all ivs be represented by a signed integer?
   As CLooG might generate negative values in its expressions, signed loop ivs
   are required in the backend. */

static bool
scop_ivs_can_be_represented (scop_p scop)
{
  loop_iterator li;
  loop_p loop;
  gimple_stmt_iterator psi;

  FOR_EACH_LOOP (li, loop, 0)
    {
      if (!loop_in_sese_p (loop, SCOP_REGION (scop)))
	continue;

      for (psi = gsi_start_phis (loop->header);
	   !gsi_end_p (psi); gsi_next (&psi))
	{
	  gimple phi = gsi_stmt (psi);
	  tree res = PHI_RESULT (phi);
	  tree type = TREE_TYPE (res);

	  if (TYPE_UNSIGNED (type)
	      && TYPE_PRECISION (type) >= TYPE_PRECISION (long_long_integer_type_node))
	    return false;
	}
    }

  return true;
}

/* Builds the polyhedral representation for a SESE region.  */

void
build_poly_scop (scop_p scop)
{
  sese region = SCOP_REGION (scop);
  graphite_dim_t max_dim;

  build_scop_bbs (scop);

  /* FIXME: This restriction is needed to avoid a problem in CLooG.
     Once CLooG is fixed, remove this guard.  Anyways, it makes no
     sense to optimize a scop containing only PBBs that do not belong
     to any loops.  */
  if (nb_pbbs_in_loops (scop) == 0)
    return;

  if (!scop_ivs_can_be_represented (scop))
    return;

  if (flag_associative_math)
    rewrite_commutative_reductions_out_of_ssa (scop);

  build_sese_loop_nests (region);
  build_sese_conditions (region);
  find_scop_parameters (scop);

  max_dim = PARAM_VALUE (PARAM_GRAPHITE_MAX_NB_SCOP_PARAMS);
  if (scop_nb_params (scop) > max_dim)
    return;

  build_scop_iteration_domain (scop);
  build_scop_context (scop);
  add_conditions_to_constraints (scop);

  /* Rewrite out of SSA only after having translated the
     representation to the polyhedral representation to avoid scev
     analysis failures.  That means that these functions will insert
     new data references that they create in the right place.  */
  rewrite_reductions_out_of_ssa (scop);
  rewrite_cross_bb_scalar_deps_out_of_ssa (scop);

  build_scop_drs (scop);
  scop_to_lst (scop);
  build_scop_scattering (scop);

  /* This SCoP has been translated to the polyhedral
     representation.  */
  POLY_SCOP_P (scop) = true;
}
#endif
