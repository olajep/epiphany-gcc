/* Integrated Register Allocator (IRA) entry point.
   Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011
   Free Software Foundation, Inc.
   Contributed by Vladimir Makarov <vmakarov@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* The integrated register allocator (IRA) is a
   regional register allocator performing graph coloring on a top-down
   traversal of nested regions.  Graph coloring in a region is based
   on Chaitin-Briggs algorithm.  It is called integrated because
   register coalescing, register live range splitting, and choosing a
   better hard register are done on-the-fly during coloring.  Register
   coalescing and choosing a cheaper hard register is done by hard
   register preferencing during hard register assigning.  The live
   range splitting is a byproduct of the regional register allocation.

   Major IRA notions are:

     o *Region* is a part of CFG where graph coloring based on
       Chaitin-Briggs algorithm is done.  IRA can work on any set of
       nested CFG regions forming a tree.  Currently the regions are
       the entire function for the root region and natural loops for
       the other regions.  Therefore data structure representing a
       region is called loop_tree_node.

     o *Allocno class* is a register class used for allocation of
       given allocno.  It means that only hard register of given
       register class can be assigned to given allocno.  In reality,
       even smaller subset of (*profitable*) hard registers can be
       assigned.  In rare cases, the subset can be even smaller
       because our modification of Chaitin-Briggs algorithm requires
       that sets of hard registers can be assigned to allocnos forms a
       forest, i.e. the sets can be ordered in a way where any
       previous set is not intersected with given set or is a superset
       of given set.

     o *Pressure class* is a register class belonging to a set of
       register classes containing all of the hard-registers available
       for register allocation.  The set of all pressure classes for a
       target is defined in the corresponding machine-description file
       according some criteria.  Register pressure is calculated only
       for pressure classes and it affects some IRA decisions as
       forming allocation regions.

     o *Allocno* represents the live range of a pseudo-register in a
       region.  Besides the obvious attributes like the corresponding
       pseudo-register number, allocno class, conflicting allocnos and
       conflicting hard-registers, there are a few allocno attributes
       which are important for understanding the allocation algorithm:

       - *Live ranges*.  This is a list of ranges of *program points*
         where the allocno lives.  Program points represent places
         where a pseudo can be born or become dead (there are
         approximately two times more program points than the insns)
         and they are represented by integers starting with 0.  The
         live ranges are used to find conflicts between allocnos.
         They also play very important role for the transformation of
         the IRA internal representation of several regions into a one
         region representation.  The later is used during the reload
         pass work because each allocno represents all of the
         corresponding pseudo-registers.

       - *Hard-register costs*.  This is a vector of size equal to the
         number of available hard-registers of the allocno class.  The
         cost of a callee-clobbered hard-register for an allocno is
         increased by the cost of save/restore code around the calls
         through the given allocno's life.  If the allocno is a move
         instruction operand and another operand is a hard-register of
         the allocno class, the cost of the hard-register is decreased
         by the move cost.

         When an allocno is assigned, the hard-register with minimal
         full cost is used.  Initially, a hard-register's full cost is
         the corresponding value from the hard-register's cost vector.
         If the allocno is connected by a *copy* (see below) to
         another allocno which has just received a hard-register, the
         cost of the hard-register is decreased.  Before choosing a
         hard-register for an allocno, the allocno's current costs of
         the hard-registers are modified by the conflict hard-register
         costs of all of the conflicting allocnos which are not
         assigned yet.

       - *Conflict hard-register costs*.  This is a vector of the same
         size as the hard-register costs vector.  To permit an
         unassigned allocno to get a better hard-register, IRA uses
         this vector to calculate the final full cost of the
         available hard-registers.  Conflict hard-register costs of an
         unassigned allocno are also changed with a change of the
         hard-register cost of the allocno when a copy involving the
         allocno is processed as described above.  This is done to
         show other unassigned allocnos that a given allocno prefers
         some hard-registers in order to remove the move instruction
         corresponding to the copy.

     o *Cap*.  If a pseudo-register does not live in a region but
       lives in a nested region, IRA creates a special allocno called
       a cap in the outer region.  A region cap is also created for a
       subregion cap.

     o *Copy*.  Allocnos can be connected by copies.  Copies are used
       to modify hard-register costs for allocnos during coloring.
       Such modifications reflects a preference to use the same
       hard-register for the allocnos connected by copies.  Usually
       copies are created for move insns (in this case it results in
       register coalescing).  But IRA also creates copies for operands
       of an insn which should be assigned to the same hard-register
       due to constraints in the machine description (it usually
       results in removing a move generated in reload to satisfy
       the constraints) and copies referring to the allocno which is
       the output operand of an instruction and the allocno which is
       an input operand dying in the instruction (creation of such
       copies results in less register shuffling).  IRA *does not*
       create copies between the same register allocnos from different
       regions because we use another technique for propagating
       hard-register preference on the borders of regions.

   Allocnos (including caps) for the upper region in the region tree
   *accumulate* information important for coloring from allocnos with
   the same pseudo-register from nested regions.  This includes
   hard-register and memory costs, conflicts with hard-registers,
   allocno conflicts, allocno copies and more.  *Thus, attributes for
   allocnos in a region have the same values as if the region had no
   subregions*.  It means that attributes for allocnos in the
   outermost region corresponding to the function have the same values
   as though the allocation used only one region which is the entire
   function.  It also means that we can look at IRA work as if the
   first IRA did allocation for all function then it improved the
   allocation for loops then their subloops and so on.

   IRA major passes are:

     o Building IRA internal representation which consists of the
       following subpasses:

       * First, IRA builds regions and creates allocnos (file
         ira-build.c) and initializes most of their attributes.

       * Then IRA finds an allocno class for each allocno and
         calculates its initial (non-accumulated) cost of memory and
         each hard-register of its allocno class (file ira-cost.c).

       * IRA creates live ranges of each allocno, calulates register
         pressure for each pressure class in each region, sets up
         conflict hard registers for each allocno and info about calls
         the allocno lives through (file ira-lives.c).

       * IRA removes low register pressure loops from the regions
         mostly to speed IRA up (file ira-build.c).

       * IRA propagates accumulated allocno info from lower region
         allocnos to corresponding upper region allocnos (file
         ira-build.c).

       * IRA creates all caps (file ira-build.c).

       * Having live-ranges of allocnos and their classes, IRA creates
         conflicting allocnos for each allocno.  Conflicting allocnos
         are stored as a bit vector or array of pointers to the
         conflicting allocnos whatever is more profitable (file
         ira-conflicts.c).  At this point IRA creates allocno copies.

     o Coloring.  Now IRA has all necessary info to start graph coloring
       process.  It is done in each region on top-down traverse of the
       region tree (file ira-color.c).  There are following subpasses:

       * Finding profitable hard registers of corresponding allocno
         class for each allocno.  For example, only callee-saved hard
         registers are frequently profitable for allocnos living
         through colors.  If the profitable hard register set of
         allocno does not form a tree based on subset relation, we use
         some approximation to form the tree.  This approximation is
         used to figure out trivial colorability of allocnos.  The
         approximation is a pretty rare case.

       * Putting allocnos onto the coloring stack.  IRA uses Briggs
         optimistic coloring which is a major improvement over
         Chaitin's coloring.  Therefore IRA does not spill allocnos at
         this point.  There is some freedom in the order of putting
         allocnos on the stack which can affect the final result of
         the allocation.  IRA uses some heuristics to improve the
         order.
	 
	 We also use a modification of Chaitin-Briggs algorithm which
         works for intersected register classes of allocnos.  To
         figure out trivial colorability of allocnos, the mentioned
         above tree of hard register sets is used.  To get an idea how
         the algorithm works in i386 example, let us consider an
         allocno to which any general hard register can be assigned.
         If the allocno conflicts with eight allocnos to which only
         EAX register can be assigned, given allocno is still
         trivially colorable because all conflicting allocnos might be
         assigned only to EAX and all other general hard registers are
         still free.

	 To get an idea of the used trivial colorability criterion, it
	 is also useful to read article "Graph-Coloring Register
	 Allocation for Irregular Architectures" by Michael D. Smith
	 and Glen Holloway.  Major difference between the article
	 approach and approach used in IRA is that Smith's approach
	 takes register classes only from machine description and IRA
	 calculate register classes from intermediate code too
	 (e.g. an explicit usage of hard registers in RTL code for
	 parameter passing can result in creation of additional
	 register classes which contain or exclude the hard
	 registers).  That makes IRA approach useful for improving
	 coloring even for architectures with regular register files
	 and in fact some benchmarking shows the improvement for
	 regular class architectures is even bigger than for irregular
	 ones.  Another difference is that Smith's approach chooses
	 intersection of classes of all insn operands in which a given
	 pseudo occurs.  IRA can use bigger classes if it is still
	 more profitable than memory usage.

       * Popping the allocnos from the stack and assigning them hard
         registers.  If IRA can not assign a hard register to an
         allocno and the allocno is coalesced, IRA undoes the
         coalescing and puts the uncoalesced allocnos onto the stack in
         the hope that some such allocnos will get a hard register
         separately.  If IRA fails to assign hard register or memory
         is more profitable for it, IRA spills the allocno.  IRA
         assigns the allocno the hard-register with minimal full
         allocation cost which reflects the cost of usage of the
         hard-register for the allocno and cost of usage of the
         hard-register for allocnos conflicting with given allocno.

       * Chaitin-Briggs coloring assigns as many pseudos as possible
         to hard registers.  After coloringh we try to improve
         allocation with cost point of view.  We improve the
         allocation by spilling some allocnos and assigning the freed
         hard registers to other allocnos if it decreases the overall
         allocation cost.

       * After allono assigning in the region, IRA modifies the hard
         register and memory costs for the corresponding allocnos in
         the subregions to reflect the cost of possible loads, stores,
         or moves on the border of the region and its subregions.
         When default regional allocation algorithm is used
         (-fira-algorithm=mixed), IRA just propagates the assignment
         for allocnos if the register pressure in the region for the
         corresponding pressure class is less than number of available
         hard registers for given pressure class.

     o Spill/restore code moving.  When IRA performs an allocation
       by traversing regions in top-down order, it does not know what
       happens below in the region tree.  Therefore, sometimes IRA
       misses opportunities to perform a better allocation.  A simple
       optimization tries to improve allocation in a region having
       subregions and containing in another region.  If the
       corresponding allocnos in the subregion are spilled, it spills
       the region allocno if it is profitable.  The optimization
       implements a simple iterative algorithm performing profitable
       transformations while they are still possible.  It is fast in
       practice, so there is no real need for a better time complexity
       algorithm.

     o Code change.  After coloring, two allocnos representing the
       same pseudo-register outside and inside a region respectively
       may be assigned to different locations (hard-registers or
       memory).  In this case IRA creates and uses a new
       pseudo-register inside the region and adds code to move allocno
       values on the region's borders.  This is done during top-down
       traversal of the regions (file ira-emit.c).  In some
       complicated cases IRA can create a new allocno to move allocno
       values (e.g. when a swap of values stored in two hard-registers
       is needed).  At this stage, the new allocno is marked as
       spilled.  IRA still creates the pseudo-register and the moves
       on the region borders even when both allocnos were assigned to
       the same hard-register.  If the reload pass spills a
       pseudo-register for some reason, the effect will be smaller
       because another allocno will still be in the hard-register.  In
       most cases, this is better then spilling both allocnos.  If
       reload does not change the allocation for the two
       pseudo-registers, the trivial move will be removed by
       post-reload optimizations.  IRA does not generate moves for
       allocnos assigned to the same hard register when the default
       regional allocation algorithm is used and the register pressure
       in the region for the corresponding pressure class is less than
       number of available hard registers for given pressure class.
       IRA also does some optimizations to remove redundant stores and
       to reduce code duplication on the region borders.

     o Flattening internal representation.  After changing code, IRA
       transforms its internal representation for several regions into
       one region representation (file ira-build.c).  This process is
       called IR flattening.  Such process is more complicated than IR
       rebuilding would be, but is much faster.

     o After IR flattening, IRA tries to assign hard registers to all
       spilled allocnos.  This is impelemented by a simple and fast
       priority coloring algorithm (see function
       ira_reassign_conflict_allocnos::ira-color.c).  Here new allocnos
       created during the code change pass can be assigned to hard
       registers.

     o At the end IRA calls the reload pass.  The reload pass
       communicates with IRA through several functions in file
       ira-color.c to improve its decisions in

       * sharing stack slots for the spilled pseudos based on IRA info
         about pseudo-register conflicts.

       * reassigning hard-registers to all spilled pseudos at the end
         of each reload iteration.

       * choosing a better hard-register to spill based on IRA info
         about pseudo-register live ranges and the register pressure
         in places where the pseudo-register lives.

   IRA uses a lot of data representing the target processors.  These
   data are initilized in file ira.c.

   If function has no loops (or the loops are ignored when
   -fira-algorithm=CB is used), we have classic Chaitin-Briggs
   coloring (only instead of separate pass of coalescing, we use hard
   register preferencing).  In such case, IRA works much faster
   because many things are not made (like IR flattening, the
   spill/restore optimization, and the code change).

   Literature is worth to read for better understanding the code:

   o Preston Briggs, Keith D. Cooper, Linda Torczon.  Improvements to
     Graph Coloring Register Allocation.

   o David Callahan, Brian Koblenz.  Register allocation via
     hierarchical graph coloring.

   o Keith Cooper, Anshuman Dasgupta, Jason Eckhardt. Revisiting Graph
     Coloring Register Allocation: A Study of the Chaitin-Briggs and
     Callahan-Koblenz Algorithms.

   o Guei-Yuan Lueh, Thomas Gross, and Ali-Reza Adl-Tabatabai. Global
     Register Allocation Based on Graph Fusion.

   o Michael D. Smith and Glenn Holloway.  Graph-Coloring Register
     Allocation for Irregular Architectures

   o Vladimir Makarov. The Integrated Register Allocator for GCC.

   o Vladimir Makarov.  The top-down register allocator for irregular
     register file architectures.

*/


#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "regs.h"
#include "rtl.h"
#include "tm_p.h"
#include "target.h"
#include "flags.h"
#include "obstack.h"
#include "bitmap.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "df.h"
#include "expr.h"
#include "recog.h"
#include "params.h"
#include "timevar.h"
#include "tree-pass.h"
#include "output.h"
#include "except.h"
#include "reload.h"
#include "diagnostic-core.h"
#include "integrate.h"
#include "ggc.h"
#include "ira-int.h"
#include "dce.h"


struct target_ira default_target_ira;
struct target_ira_int default_target_ira_int;
#if SWITCHABLE_TARGET
struct target_ira *this_target_ira = &default_target_ira;
struct target_ira_int *this_target_ira_int = &default_target_ira_int;
#endif

/* A modified value of flag `-fira-verbose' used internally.  */
int internal_flag_ira_verbose;

/* Dump file of the allocator if it is not NULL.  */
FILE *ira_dump_file;

/* The number of elements in the following array.  */
int ira_spilled_reg_stack_slots_num;

/* The following array contains info about spilled pseudo-registers
   stack slots used in current function so far.  */
struct ira_spilled_reg_stack_slot *ira_spilled_reg_stack_slots;

/* Correspondingly overall cost of the allocation, overall cost before
   reload, cost of the allocnos assigned to hard-registers, cost of
   the allocnos assigned to memory, cost of loads, stores and register
   move insns generated for pseudo-register live range splitting (see
   ira-emit.c).  */
int ira_overall_cost, overall_cost_before;
int ira_reg_cost, ira_mem_cost;
int ira_load_cost, ira_store_cost, ira_shuffle_cost;
int ira_move_loops_num, ira_additional_jumps_num;

/* All registers that can be eliminated.  */

HARD_REG_SET eliminable_regset;

/* Temporary hard reg set used for a different calculation.  */
static HARD_REG_SET temp_hard_regset;



/* The function sets up the map IRA_REG_MODE_HARD_REGSET.  */
static void
setup_reg_mode_hard_regset (void)
{
  int i, m, hard_regno;

  for (m = 0; m < NUM_MACHINE_MODES; m++)
    for (hard_regno = 0; hard_regno < FIRST_PSEUDO_REGISTER; hard_regno++)
      {
	CLEAR_HARD_REG_SET (ira_reg_mode_hard_regset[hard_regno][m]);
	for (i = hard_regno_nregs[hard_regno][m] - 1; i >= 0; i--)
	  if (hard_regno + i < FIRST_PSEUDO_REGISTER)
	    SET_HARD_REG_BIT (ira_reg_mode_hard_regset[hard_regno][m],
			      hard_regno + i);
      }
}


#define no_unit_alloc_regs \
  (this_target_ira_int->x_no_unit_alloc_regs)

/* The function sets up the three arrays declared above.  */
static void
setup_class_hard_regs (void)
{
  int cl, i, hard_regno, n;
  HARD_REG_SET processed_hard_reg_set;

  ira_assert (SHRT_MAX >= FIRST_PSEUDO_REGISTER);
  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    {
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      CLEAR_HARD_REG_SET (processed_hard_reg_set);
      for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	{
	  ira_non_ordered_class_hard_regs[cl][i] = -1;
	  ira_class_hard_reg_index[cl][i] = -1;
	}
      for (n = 0, i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	{
#ifdef REG_ALLOC_ORDER
	  hard_regno = reg_alloc_order[i];
#else
	  hard_regno = i;
#endif
	  if (TEST_HARD_REG_BIT (processed_hard_reg_set, hard_regno))
	    continue;
	  SET_HARD_REG_BIT (processed_hard_reg_set, hard_regno);
      	  if (! TEST_HARD_REG_BIT (temp_hard_regset, hard_regno))
	    ira_class_hard_reg_index[cl][hard_regno] = -1;
	  else
	    {
	      ira_class_hard_reg_index[cl][hard_regno] = n;
	      ira_class_hard_regs[cl][n++] = hard_regno;
	    }
	}
      ira_class_hard_regs_num[cl] = n;
      for (n = 0, i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	if (TEST_HARD_REG_BIT (temp_hard_regset, i))
	  ira_non_ordered_class_hard_regs[cl][n++] = i;
      ira_assert (ira_class_hard_regs_num[cl] == n);
    }
}

/* Set up IRA_AVAILABLE_CLASS_REGS.  */
static void
setup_available_class_regs (void)
{
  int i, j;

  memset (ira_available_class_regs, 0, sizeof (ira_available_class_regs));
  for (i = 0; i < N_REG_CLASSES; i++)
    {
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[i]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      for (j = 0; j < FIRST_PSEUDO_REGISTER; j++)
	if (TEST_HARD_REG_BIT (temp_hard_regset, j))
	  ira_available_class_regs[i]++;
    }
}

/* Set up global variables defining info about hard registers for the
   allocation.  These depend on USE_HARD_FRAME_P whose TRUE value means
   that we can use the hard frame pointer for the allocation.  */
static void
setup_alloc_regs (bool use_hard_frame_p)
{
#ifdef ADJUST_REG_ALLOC_ORDER
  ADJUST_REG_ALLOC_ORDER;
#endif
  COPY_HARD_REG_SET (no_unit_alloc_regs, fixed_reg_set);
  if (! use_hard_frame_p)
    SET_HARD_REG_BIT (no_unit_alloc_regs, HARD_FRAME_POINTER_REGNUM);
  setup_class_hard_regs ();
  setup_available_class_regs ();
}



#define alloc_reg_class_subclasses \
  (this_target_ira_int->x_alloc_reg_class_subclasses)

/* Initialize the table of subclasses of each reg class.  */
static void
setup_reg_subclasses (void)
{
  int i, j;
  HARD_REG_SET temp_hard_regset2;

  for (i = 0; i < N_REG_CLASSES; i++)
    for (j = 0; j < N_REG_CLASSES; j++)
      alloc_reg_class_subclasses[i][j] = LIM_REG_CLASSES;

  for (i = 0; i < N_REG_CLASSES; i++)
    {
      if (i == (int) NO_REGS)
	continue;

      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[i]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      if (hard_reg_set_empty_p (temp_hard_regset))
	continue;
      for (j = 0; j < N_REG_CLASSES; j++)
	if (i != j)
	  {
	    enum reg_class *p;

	    COPY_HARD_REG_SET (temp_hard_regset2, reg_class_contents[j]);
	    AND_COMPL_HARD_REG_SET (temp_hard_regset2, no_unit_alloc_regs);
	    if (! hard_reg_set_subset_p (temp_hard_regset,
					 temp_hard_regset2))
	      continue;
	    p = &alloc_reg_class_subclasses[j][0];
	    while (*p != LIM_REG_CLASSES) p++;
	    *p = (enum reg_class) i;
	  }
    }
}



/* Set up IRA_MEMORY_MOVE_COST and IRA_MAX_MEMORY_MOVE_COST.  */
static void
setup_class_subset_and_memory_move_costs (void)
{
  int cl, cl2, mode, cost;
  HARD_REG_SET temp_hard_regset2;

  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
    ira_memory_move_cost[mode][NO_REGS][0]
      = ira_memory_move_cost[mode][NO_REGS][1] = SHRT_MAX;
  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    {
      if (cl != (int) NO_REGS)
	for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
	  {
	    ira_max_memory_move_cost[mode][cl][0]
	      = ira_memory_move_cost[mode][cl][0]
	      = memory_move_cost ((enum machine_mode) mode,
				  (reg_class_t) cl, false);
	    ira_max_memory_move_cost[mode][cl][1]
	      = ira_memory_move_cost[mode][cl][1]
	      = memory_move_cost ((enum machine_mode) mode,
				  (reg_class_t) cl, true);
	    /* Costs for NO_REGS are used in cost calculation on the
	       1st pass when the preferred register classes are not
	       known yet.  In this case we take the best scenario.  */
	    if (ira_memory_move_cost[mode][NO_REGS][0]
		> ira_memory_move_cost[mode][cl][0])
	      ira_max_memory_move_cost[mode][NO_REGS][0]
		= ira_memory_move_cost[mode][NO_REGS][0]
		= ira_memory_move_cost[mode][cl][0];
	    if (ira_memory_move_cost[mode][NO_REGS][1]
		> ira_memory_move_cost[mode][cl][1])
	      ira_max_memory_move_cost[mode][NO_REGS][1]
		= ira_memory_move_cost[mode][NO_REGS][1]
		= ira_memory_move_cost[mode][cl][1];
	  }
    }
  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    for (cl2 = (int) N_REG_CLASSES - 1; cl2 >= 0; cl2--)
      {
	COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
	AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	COPY_HARD_REG_SET (temp_hard_regset2, reg_class_contents[cl2]);
	AND_COMPL_HARD_REG_SET (temp_hard_regset2, no_unit_alloc_regs);
	ira_class_subset_p[cl][cl2]
	  = hard_reg_set_subset_p (temp_hard_regset, temp_hard_regset2);
	if (! hard_reg_set_empty_p (temp_hard_regset2)
	    && hard_reg_set_subset_p (reg_class_contents[cl2],
				      reg_class_contents[cl]))
	  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
	    {
	      cost = ira_memory_move_cost[mode][cl2][0];
	      if (cost > ira_max_memory_move_cost[mode][cl][0])
		ira_max_memory_move_cost[mode][cl][0] = cost;
	      cost = ira_memory_move_cost[mode][cl2][1];
	      if (cost > ira_max_memory_move_cost[mode][cl][1])
		ira_max_memory_move_cost[mode][cl][1] = cost;
	    }
      }
  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
      {
	ira_memory_move_cost[mode][cl][0]
	  = ira_max_memory_move_cost[mode][cl][0];
	ira_memory_move_cost[mode][cl][1]
	  = ira_max_memory_move_cost[mode][cl][1];
      }
  setup_reg_subclasses ();
}



/* Define the following macro if allocation through malloc if
   preferable.  */
#define IRA_NO_OBSTACK

#ifndef IRA_NO_OBSTACK
/* Obstack used for storing all dynamic data (except bitmaps) of the
   IRA.  */
static struct obstack ira_obstack;
#endif

/* Obstack used for storing all bitmaps of the IRA.  */
static struct bitmap_obstack ira_bitmap_obstack;

/* Allocate memory of size LEN for IRA data.  */
void *
ira_allocate (size_t len)
{
  void *res;

#ifndef IRA_NO_OBSTACK
  res = obstack_alloc (&ira_obstack, len);
#else
  res = xmalloc (len);
#endif
  return res;
}

/* Free memory ADDR allocated for IRA data.  */
void
ira_free (void *addr ATTRIBUTE_UNUSED)
{
#ifndef IRA_NO_OBSTACK
  /* do nothing */
#else
  free (addr);
#endif
}


/* Allocate and returns bitmap for IRA.  */
bitmap
ira_allocate_bitmap (void)
{
  return BITMAP_ALLOC (&ira_bitmap_obstack);
}

/* Free bitmap B allocated for IRA.  */
void
ira_free_bitmap (bitmap b ATTRIBUTE_UNUSED)
{
  /* do nothing */
}



/* Output information about allocation of all allocnos (except for
   caps) into file F.  */
void
ira_print_disposition (FILE *f)
{
  int i, n, max_regno;
  ira_allocno_t a;
  basic_block bb;

  fprintf (f, "Disposition:");
  max_regno = max_reg_num ();
  for (n = 0, i = FIRST_PSEUDO_REGISTER; i < max_regno; i++)
    for (a = ira_regno_allocno_map[i];
	 a != NULL;
	 a = ALLOCNO_NEXT_REGNO_ALLOCNO (a))
      {
	if (n % 4 == 0)
	  fprintf (f, "\n");
	n++;
	fprintf (f, " %4d:r%-4d", ALLOCNO_NUM (a), ALLOCNO_REGNO (a));
	if ((bb = ALLOCNO_LOOP_TREE_NODE (a)->bb) != NULL)
	  fprintf (f, "b%-3d", bb->index);
	else
	  fprintf (f, "l%-3d", ALLOCNO_LOOP_TREE_NODE (a)->loop_num);
	if (ALLOCNO_HARD_REGNO (a) >= 0)
	  fprintf (f, " %3d", ALLOCNO_HARD_REGNO (a));
	else
	  fprintf (f, " mem");
      }
  fprintf (f, "\n");
}

/* Outputs information about allocation of all allocnos into
   stderr.  */
void
ira_debug_disposition (void)
{
  ira_print_disposition (stderr);
}



/* Set up ira_stack_reg_pressure_class which is the biggest pressure
   register class containing stack registers or NO_REGS if there are
   no stack registers.  To find this class, we iterate through all
   register pressure classes and choose the first register pressure
   class containing all the stack registers and having the biggest
   size.  */
static void
setup_stack_reg_pressure_class (void)
{
  ira_stack_reg_pressure_class = NO_REGS;
#ifdef STACK_REGS
  {
    int i, best, size;
    enum reg_class cl;
    HARD_REG_SET temp_hard_regset2;

    CLEAR_HARD_REG_SET (temp_hard_regset);
    for (i = FIRST_STACK_REG; i <= LAST_STACK_REG; i++)
      SET_HARD_REG_BIT (temp_hard_regset, i);
    best = 0;
    for (i = 0; i < ira_pressure_classes_num; i++)
      {
	cl = ira_pressure_classes[i];
	COPY_HARD_REG_SET (temp_hard_regset2, temp_hard_regset);
	AND_HARD_REG_SET (temp_hard_regset2, reg_class_contents[cl]);
	size = hard_reg_set_size (temp_hard_regset2);
	if (best < size)
	  {
	    best = size;
	    ira_stack_reg_pressure_class = cl;
	  }
      }
  }
#endif
}

/* Find pressure classes which are register classes for which we
   calculate register pressure in IRA, register pressure sensitive
   insn scheduling, and register pressure sensitive loop invariant
   motion.

   To make register pressure calculation easy, we always use
   non-intersected register pressure classes.  A move of hard
   registers from one register pressure class is not more expensive
   than load and store of the hard registers.  Most likely an allocno
   class will be a subset of a register pressure class and in many
   cases a register pressure class.  That makes usage of register
   pressure classes a good approximation to find a high register
   pressure.  */
static void
setup_pressure_classes (void)
{
  int cost, i, n, curr;
  int cl, cl2;
  enum reg_class pressure_classes[N_REG_CLASSES];
  int m;
  HARD_REG_SET temp_hard_regset2;
  bool insert_p;

  n = 0;
  for (cl = 0; cl < N_REG_CLASSES; cl++)
    {
      if (ira_available_class_regs[cl] == 0)
	continue;
      if (ira_available_class_regs[cl] != 1
	  /* A register class without subclasses may contain a few
	     hard registers and movement between them is costly
	     (e.g. SPARC FPCC registers).  We still should consider it
	     as a candidate for a pressure class.  */
	  && alloc_reg_class_subclasses[cl][0] != LIM_REG_CLASSES)
	{
	  /* Check that the moves between any hard registers of the
	     current class are not more expensive for a legal mode
	     than load/store of the hard registers of the current
	     class.  Such class is a potential candidate to be a
	     register pressure class.  */
	  for (m = 0; m < NUM_MACHINE_MODES; m++)
	    {
	      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
	      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	      AND_COMPL_HARD_REG_SET (temp_hard_regset,
				      ira_prohibited_class_mode_regs[cl][m]);
	      if (hard_reg_set_empty_p (temp_hard_regset))
		continue;
	      ira_init_register_move_cost_if_necessary ((enum machine_mode) m);
	      cost = ira_register_move_cost[m][cl][cl];
	      if (cost <= ira_max_memory_move_cost[m][cl][1]
		  || cost <= ira_max_memory_move_cost[m][cl][0])
		break;
	    }
	  if (m >= NUM_MACHINE_MODES)
	    continue;
	}
      curr = 0;
      insert_p = true;
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      /* Remove so far added pressure classes which are subset of the
	 current candidate class.  Prefer GENERAL_REGS as a pressure
	 register class to another class containing the same
	 allocatable hard registers.  We do this because machine
	 dependent cost hooks might give wrong costs for the latter
	 class but always give the right cost for the former class
	 (GENERAL_REGS).  */
      for (i = 0; i < n; i++)
	{
	  cl2 = pressure_classes[i];
	  COPY_HARD_REG_SET (temp_hard_regset2, reg_class_contents[cl2]);
	  AND_COMPL_HARD_REG_SET (temp_hard_regset2, no_unit_alloc_regs);
	  if (hard_reg_set_subset_p (temp_hard_regset, temp_hard_regset2)
	      && (! hard_reg_set_equal_p (temp_hard_regset, temp_hard_regset2)
		  || cl2 == (int) GENERAL_REGS))
	    {
	      pressure_classes[curr++] = (enum reg_class) cl2;
	      insert_p = false;
	      continue;
	    }
	  if (hard_reg_set_subset_p (temp_hard_regset2, temp_hard_regset)
	      && (! hard_reg_set_equal_p (temp_hard_regset2, temp_hard_regset)
		  || cl == (int) GENERAL_REGS))
	    continue;
	  if (hard_reg_set_equal_p (temp_hard_regset2, temp_hard_regset))
	    insert_p = false;
	  pressure_classes[curr++] = (enum reg_class) cl2;
	}
      /* If the current candidate is a subset of a so far added
	 pressure class, don't add it to the list of the pressure
	 classes.  */
      if (insert_p)
	pressure_classes[curr++] = (enum reg_class) cl;
      n = curr;
    }
#ifdef ENABLE_IRA_CHECKING
  {
    HARD_REG_SET ignore_hard_regs;

    /* Check pressure classes correctness: here we check that hard
       registers from all register pressure classes contains all hard
       registers available for the allocation.  */
    CLEAR_HARD_REG_SET (temp_hard_regset);
    CLEAR_HARD_REG_SET (temp_hard_regset2);
    COPY_HARD_REG_SET (ignore_hard_regs, no_unit_alloc_regs);
    for (cl = 0; cl < LIM_REG_CLASSES; cl++)
      {
	/* For some targets (like MIPS with MD_REGS), there are some
	   classes with hard registers available for allocation but
	   not able to hold value of any mode.  */
	for (m = 0; m < NUM_MACHINE_MODES; m++)
	  if (contains_reg_of_mode[cl][m])
	    break;
	if (m >= NUM_MACHINE_MODES)
	  {
	    IOR_HARD_REG_SET (ignore_hard_regs, reg_class_contents[cl]);
	    continue;
	  }
	for (i = 0; i < n; i++)
	  if ((int) pressure_classes[i] == cl)
	    break;
	IOR_HARD_REG_SET (temp_hard_regset2, reg_class_contents[cl]);
	if (i < n)
	  IOR_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
      }
    for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
      /* Some targets (like SPARC with ICC reg) have alocatable regs
	 for which no reg class is defined.  */
      if (REGNO_REG_CLASS (i) == NO_REGS)
	SET_HARD_REG_BIT (ignore_hard_regs, i);
    AND_COMPL_HARD_REG_SET (temp_hard_regset, ignore_hard_regs);
    AND_COMPL_HARD_REG_SET (temp_hard_regset2, ignore_hard_regs);
    ira_assert (hard_reg_set_subset_p (temp_hard_regset2, temp_hard_regset));
  }
#endif
  ira_pressure_classes_num = 0;
  for (i = 0; i < n; i++)
    {
      cl = (int) pressure_classes[i];
      ira_reg_pressure_class_p[cl] = true;
      ira_pressure_classes[ira_pressure_classes_num++] = (enum reg_class) cl;
    }
  setup_stack_reg_pressure_class ();
}

/* Set up IRA_ALLOCNO_CLASSES, IRA_ALLOCNO_CLASSES_NUM,
   IRA_IMPORTANT_CLASSES, and IRA_IMPORTANT_CLASSES_NUM.

   Target may have many subtargets and not all target hard regiters can
   be used for allocation, e.g. x86 port in 32-bit mode can not use
   hard registers introduced in x86-64 like r8-r15).  Some classes
   might have the same allocatable hard registers, e.g.  INDEX_REGS
   and GENERAL_REGS in x86 port in 32-bit mode.  To decrease different
   calculations efforts we introduce allocno classes which contain
   unique non-empty sets of allocatable hard-registers.

   Pseudo class cost calculation in ira-costs.c is very expensive.
   Therefore we are trying to decrease number of classes involved in
   such calculation.  Register classes used in the cost calculation
   are called important classes.  They are allocno classes and other
   non-empty classes whose allocatable hard register sets are inside
   of an allocno class hard register set.  From the first sight, it
   looks like that they are just allocno classes.  It is not true.  In
   example of x86-port in 32-bit mode, allocno classes will contain
   GENERAL_REGS but not LEGACY_REGS (because allocatable hard
   registers are the same for the both classes).  The important
   classes will contain GENERAL_REGS and LEGACY_REGS.  It is done
   because a machine description insn constraint may refers for
   LEGACY_REGS and code in ira-costs.c is mostly base on investigation
   of the insn constraints.  */
static void
setup_allocno_and_important_classes (void)
{
  int i, j, n, cl;
  bool set_p;
  HARD_REG_SET temp_hard_regset2;
  static enum reg_class classes[LIM_REG_CLASSES + 1];

  n = 0;
  /* Collect classes which contain unique sets of allocatable hard
     registers.  Prefer GENERAL_REGS to other classes containing the
     same set of hard registers.  */
  for (i = 0; i < LIM_REG_CLASSES; i++)
    {
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[i]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      for (j = 0; j < n; j++)
	{
	  cl = classes[j];
	  COPY_HARD_REG_SET (temp_hard_regset2, reg_class_contents[cl]);
	  AND_COMPL_HARD_REG_SET (temp_hard_regset2,
				  no_unit_alloc_regs);
	  if (hard_reg_set_equal_p (temp_hard_regset,
				    temp_hard_regset2))
	    break;
	}
      if (j >= n)
	classes[n++] = (enum reg_class) i;
      else if (i == GENERAL_REGS)
	/* Prefer general regs.  For i386 example, it means that
	   we prefer GENERAL_REGS over INDEX_REGS or LEGACY_REGS
	   (all of them consists of the same available hard
	   registers).  */
	classes[j] = (enum reg_class) i;
    }
  classes[n] = LIM_REG_CLASSES;

  /* Set up classes which can be used for allocnos as classes
     conatining non-empty unique sets of allocatable hard
     registers.  */
  ira_allocno_classes_num = 0;
  for (i = 0; (cl = classes[i]) != LIM_REG_CLASSES; i++)
    {
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      if (hard_reg_set_empty_p (temp_hard_regset))
	continue;
      ira_allocno_classes[ira_allocno_classes_num++] = (enum reg_class) cl;
    }
  ira_important_classes_num = 0;
  /* Add non-allocno classes containing to non-empty set of
     allocatable hard regs.  */
  for (cl = 0; cl < N_REG_CLASSES; cl++)
    {
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      if (! hard_reg_set_empty_p (temp_hard_regset))
	{
	  set_p = false;
	  for (j = 0; j < ira_allocno_classes_num; j++)
	    {
	      COPY_HARD_REG_SET (temp_hard_regset2,
				 reg_class_contents[ira_allocno_classes[j]]);
	      AND_COMPL_HARD_REG_SET (temp_hard_regset2, no_unit_alloc_regs);
	      if ((enum reg_class) cl == ira_allocno_classes[j])
		break;
	      else if (hard_reg_set_subset_p (temp_hard_regset,
					      temp_hard_regset2))
		set_p = true;
	    }
	  if (set_p && j >= ira_allocno_classes_num)
	    ira_important_classes[ira_important_classes_num++]
	      = (enum reg_class) cl;
	}
    }
  /* Now add allocno classes to the important classes.  */
  for (j = 0; j < ira_allocno_classes_num; j++)
    ira_important_classes[ira_important_classes_num++]
      = ira_allocno_classes[j];
  for (cl = 0; cl < N_REG_CLASSES; cl++)
    {
      ira_reg_allocno_class_p[cl] = false;
      ira_reg_pressure_class_p[cl] = false;
    }
  for (j = 0; j < ira_allocno_classes_num; j++)
    ira_reg_allocno_class_p[ira_allocno_classes[j]] = true;
  setup_pressure_classes ();
}

/* Setup translation in CLASS_TRANSLATE of all classes into a class
   given by array CLASSES of length CLASSES_NUM.  The function is used
   make translation any reg class to an allocno class or to an
   pressure class.  This translation is necessary for some
   calculations when we can use only allocno or pressure classes and
   such translation represents an approximate representation of all
   classes.

   The translation in case when allocatable hard register set of a
   given class is subset of allocatable hard register set of a class
   in CLASSES is pretty simple.  We use smallest classes from CLASSES
   containing a given class.  If allocatable hard register set of a
   given class is not a subset of any corresponding set of a class
   from CLASSES, we use the cheapest (with load/store point of view)
   class from CLASSES whose set intersects with given class set */
static void
setup_class_translate_array (enum reg_class *class_translate,
			     int classes_num, enum reg_class *classes)
{
  int cl, mode;
  enum reg_class aclass, best_class, *cl_ptr;
  int i, cost, min_cost, best_cost;

  for (cl = 0; cl < N_REG_CLASSES; cl++)
    class_translate[cl] = NO_REGS;

  for (i = 0; i < classes_num; i++)
    {
      aclass = classes[i];
      for (cl_ptr = &alloc_reg_class_subclasses[aclass][0];
	   (cl = *cl_ptr) != LIM_REG_CLASSES;
	   cl_ptr++)
	if (class_translate[cl] == NO_REGS)
	  class_translate[cl] = aclass;
      class_translate[aclass] = aclass;
    }
  /* For classes which are not fully covered by one of given classes
     (in other words covered by more one given class), use the
     cheapest class.  */
  for (cl = 0; cl < N_REG_CLASSES; cl++)
    {
      if (cl == NO_REGS || class_translate[cl] != NO_REGS)
	continue;
      best_class = NO_REGS;
      best_cost = INT_MAX;
      for (i = 0; i < classes_num; i++)
	{
	  aclass = classes[i];
	  COPY_HARD_REG_SET (temp_hard_regset,
			     reg_class_contents[aclass]);
	  AND_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl]);
	  AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	  if (! hard_reg_set_empty_p (temp_hard_regset))
	    {
	      min_cost = INT_MAX;
	      for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
		{
		  cost = (ira_memory_move_cost[mode][cl][0]
			  + ira_memory_move_cost[mode][cl][1]);
		  if (min_cost > cost)
		    min_cost = cost;
		}
	      if (best_class == NO_REGS || best_cost > min_cost)
		{
		  best_class = aclass;
		  best_cost = min_cost;
		}
	    }
	}
      class_translate[cl] = best_class;
    }
}

/* Set up array IRA_ALLOCNO_CLASS_TRANSLATE and
   IRA_PRESSURE_CLASS_TRANSLATE.  */
static void
setup_class_translate (void)
{
  setup_class_translate_array (ira_allocno_class_translate,
			       ira_allocno_classes_num, ira_allocno_classes);
  setup_class_translate_array (ira_pressure_class_translate,
			       ira_pressure_classes_num, ira_pressure_classes);
}

/* Order numbers of allocno classes in original target allocno class
   array, -1 for non-allocno classes.  */
static int allocno_class_order[N_REG_CLASSES];

/* The function used to sort the important classes.  */
static int
comp_reg_classes_func (const void *v1p, const void *v2p)
{
  enum reg_class cl1 = *(const enum reg_class *) v1p;
  enum reg_class cl2 = *(const enum reg_class *) v2p;
  enum reg_class tcl1, tcl2;
  int diff;

  tcl1 = ira_allocno_class_translate[cl1];
  tcl2 = ira_allocno_class_translate[cl2];
  if (tcl1 != NO_REGS && tcl2 != NO_REGS
      && (diff = allocno_class_order[tcl1] - allocno_class_order[tcl2]) != 0)
    return diff;
  return (int) cl1 - (int) cl2;
}

/* For correct work of function setup_reg_class_relation we need to
   reorder important classes according to the order of their allocno
   classes.  It places important classes containing the same
   allocatable hard register set adjacent to each other and allocno
   class with the allocatable hard register set right after the other
   important classes with the same set.

   In example from comments of function
   setup_allocno_and_important_classes, it places LEGACY_REGS and
   GENERAL_REGS close to each other and GENERAL_REGS is after
   LEGACY_REGS.  */
static void
reorder_important_classes (void)
{
  int i;

  for (i = 0; i < N_REG_CLASSES; i++)
    allocno_class_order[i] = -1;
  for (i = 0; i < ira_allocno_classes_num; i++)
    allocno_class_order[ira_allocno_classes[i]] = i;
  qsort (ira_important_classes, ira_important_classes_num,
	 sizeof (enum reg_class), comp_reg_classes_func);
  for (i = 0; i < ira_important_classes_num; i++)
    ira_important_class_nums[ira_important_classes[i]] = i;
}

/* Set up IRA_REG_CLASS_SUBUNION, IRA_REG_CLASS_SUPERUNION,
   IRA_REG_CLASS_SUPER_CLASSES, IRA_REG_CLASSES_INTERSECT, and
   IRA_REG_CLASSES_INTERSECT_P.  For the meaning of the relations,
   please see corresponding comments in ira-int.h.  */
static void
setup_reg_class_relations (void)
{
  int i, cl1, cl2, cl3;
  HARD_REG_SET intersection_set, union_set, temp_set2;
  bool important_class_p[N_REG_CLASSES];

  memset (important_class_p, 0, sizeof (important_class_p));
  for (i = 0; i < ira_important_classes_num; i++)
    important_class_p[ira_important_classes[i]] = true;
  for (cl1 = 0; cl1 < N_REG_CLASSES; cl1++)
    {
      ira_reg_class_super_classes[cl1][0] = LIM_REG_CLASSES;
      for (cl2 = 0; cl2 < N_REG_CLASSES; cl2++)
	{
	  ira_reg_classes_intersect_p[cl1][cl2] = false;
	  ira_reg_class_intersect[cl1][cl2] = NO_REGS;
	  COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl1]);
	  AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	  COPY_HARD_REG_SET (temp_set2, reg_class_contents[cl2]);
	  AND_COMPL_HARD_REG_SET (temp_set2, no_unit_alloc_regs);
	  if (hard_reg_set_empty_p (temp_hard_regset)
	      && hard_reg_set_empty_p (temp_set2))
	    {
	      /* The both classes have no allocatable hard registers
		 -- take all class hard registers into account and use
		 reg_class_subunion and reg_class_superunion.  */
	      for (i = 0;; i++)
		{
		  cl3 = reg_class_subclasses[cl1][i];
		  if (cl3 == LIM_REG_CLASSES)
		    break;
		  if (reg_class_subset_p (ira_reg_class_intersect[cl1][cl2],
					  (enum reg_class) cl3))
		    ira_reg_class_intersect[cl1][cl2] = (enum reg_class) cl3;
		}
	      ira_reg_class_subunion[cl1][cl2] = reg_class_subunion[cl1][cl2];
	      ira_reg_class_superunion[cl1][cl2] = reg_class_superunion[cl1][cl2];
	      continue;
	    }
	  ira_reg_classes_intersect_p[cl1][cl2]
	    = hard_reg_set_intersect_p (temp_hard_regset, temp_set2);
	  if (important_class_p[cl1] && important_class_p[cl2]
	      && hard_reg_set_subset_p (temp_hard_regset, temp_set2))
	    {
	      /* CL1 and CL2 are important classes and CL1 allocatable
		 hard register set is inside of CL2 allocatable hard
		 registers -- make CL1 a superset of CL2.  */
	      enum reg_class *p;

	      p = &ira_reg_class_super_classes[cl1][0];
	      while (*p != LIM_REG_CLASSES)
		p++;
	      *p++ = (enum reg_class) cl2;
	      *p = LIM_REG_CLASSES;
	    }
	  ira_reg_class_subunion[cl1][cl2] = NO_REGS;
	  ira_reg_class_superunion[cl1][cl2] = NO_REGS;
	  COPY_HARD_REG_SET (intersection_set, reg_class_contents[cl1]);
	  AND_HARD_REG_SET (intersection_set, reg_class_contents[cl2]);
	  AND_COMPL_HARD_REG_SET (intersection_set, no_unit_alloc_regs);
	  COPY_HARD_REG_SET (union_set, reg_class_contents[cl1]);
	  IOR_HARD_REG_SET (union_set, reg_class_contents[cl2]);
	  AND_COMPL_HARD_REG_SET (union_set, no_unit_alloc_regs);
	  for (i = 0; i < ira_important_classes_num; i++)
	    {
	      cl3 = ira_important_classes[i];
	      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl3]);
	      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	      if (hard_reg_set_subset_p (temp_hard_regset, intersection_set))
		{
		  /* CL3 allocatable hard register set is inside of
		     intersection of allocatable hard register sets
		     of CL1 and CL2.  */
		  COPY_HARD_REG_SET
		    (temp_set2,
		     reg_class_contents[(int)
					ira_reg_class_intersect[cl1][cl2]]);
		  AND_COMPL_HARD_REG_SET (temp_set2, no_unit_alloc_regs);
	 	  if (! hard_reg_set_subset_p (temp_hard_regset, temp_set2)
		      /* If the allocatable hard register sets are the
			 same, prefer GENERAL_REGS or the smallest
			 class for debugging purposes.  */
		      || (hard_reg_set_equal_p (temp_hard_regset, temp_set2)
			  && (cl3 == GENERAL_REGS
			      || (ira_reg_class_intersect[cl1][cl2] != GENERAL_REGS
				  && hard_reg_set_subset_p
				     (reg_class_contents[cl3],
				      reg_class_contents
				      [(int) ira_reg_class_intersect[cl1][cl2]])))))
		    ira_reg_class_intersect[cl1][cl2] = (enum reg_class) cl3;
		}
	      if (hard_reg_set_subset_p (temp_hard_regset, union_set))
		{
		  /* CL3 allocatbale hard register set is inside of
		     union of allocatable hard register sets of CL1
		     and CL2.  */
		  COPY_HARD_REG_SET
		    (temp_set2,
		     reg_class_contents[(int) ira_reg_class_subunion[cl1][cl2]]);
		  AND_COMPL_HARD_REG_SET (temp_set2, no_unit_alloc_regs);
	 	  if (ira_reg_class_subunion[cl1][cl2] == NO_REGS
		      || (hard_reg_set_subset_p (temp_set2, temp_hard_regset)
			  
			  && (! hard_reg_set_equal_p (temp_set2,
						      temp_hard_regset)
			      || cl3 == GENERAL_REGS
			      /* If the allocatable hard register sets are the
				 same, prefer GENERAL_REGS or the smallest
				 class for debugging purposes.  */
			      || (ira_reg_class_subunion[cl1][cl2] != GENERAL_REGS
				  && hard_reg_set_subset_p
				     (reg_class_contents[cl3],
				      reg_class_contents
				      [(int) ira_reg_class_subunion[cl1][cl2]])))))
		    ira_reg_class_subunion[cl1][cl2] = (enum reg_class) cl3;
		}
	      if (hard_reg_set_subset_p (union_set, temp_hard_regset))
		{
		  /* CL3 allocatable hard register set contains union
		     of allocatable hard register sets of CL1 and
		     CL2.  */
		  COPY_HARD_REG_SET
		    (temp_set2,
		     reg_class_contents[(int) ira_reg_class_superunion[cl1][cl2]]);
		  AND_COMPL_HARD_REG_SET (temp_set2, no_unit_alloc_regs);
	 	  if (ira_reg_class_superunion[cl1][cl2] == NO_REGS
		      || (hard_reg_set_subset_p (temp_hard_regset, temp_set2)

			  && (! hard_reg_set_equal_p (temp_set2,
						      temp_hard_regset)
			      || cl3 == GENERAL_REGS
			      /* If the allocatable hard register sets are the
				 same, prefer GENERAL_REGS or the smallest
				 class for debugging purposes.  */
			      || (ira_reg_class_superunion[cl1][cl2] != GENERAL_REGS
				  && hard_reg_set_subset_p
				     (reg_class_contents[cl3],
				      reg_class_contents
				      [(int) ira_reg_class_superunion[cl1][cl2]])))))
		    ira_reg_class_superunion[cl1][cl2] = (enum reg_class) cl3;
		}
	    }
	}
    }
}

/* Output all possible allocno classes and the translation map into
   file F.  */
static void
print_classes (FILE *f, bool pressure_p)
{
  int classes_num = (pressure_p
		     ? ira_pressure_classes_num : ira_allocno_classes_num);
  enum reg_class *classes = (pressure_p
			     ? ira_pressure_classes : ira_allocno_classes);
  enum reg_class *class_translate = (pressure_p
				     ? ira_pressure_class_translate
				     : ira_allocno_class_translate);
  static const char *const reg_class_names[] = REG_CLASS_NAMES;
  int i;

  fprintf (f, "%s classes:\n", pressure_p ? "Pressure" : "Allocno");
  for (i = 0; i < classes_num; i++)
    fprintf (f, " %s", reg_class_names[classes[i]]);
  fprintf (f, "\nClass translation:\n");
  for (i = 0; i < N_REG_CLASSES; i++)
    fprintf (f, " %s -> %s\n", reg_class_names[i],
	     reg_class_names[class_translate[i]]);
}

/* Output all possible allocno and translation classes and the
   translation maps into stderr.  */
void
ira_debug_allocno_classes (void)
{
  print_classes (stderr, false);
  print_classes (stderr, true);
}

/* Set up different arrays concerning class subsets, allocno and
   important classes.  */
static void
find_reg_classes (void)
{
  setup_allocno_and_important_classes ();
  setup_class_translate ();
  reorder_important_classes ();
  setup_reg_class_relations ();
}



/* Set up the array above.  */
static void
setup_hard_regno_aclass (void)
{
  int i;

  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
    {
#if 1
      ira_hard_regno_allocno_class[i]
	= (TEST_HARD_REG_BIT (no_unit_alloc_regs, i)
	   ? NO_REGS
	   : ira_allocno_class_translate[REGNO_REG_CLASS (i)]);
#else
      int j;
      enum reg_class cl;
      ira_hard_regno_allocno_class[i] = NO_REGS;
      for (j = 0; j < ira_allocno_classes_num; j++)
 	{
	  cl = ira_allocno_classes[j];
 	  if (ira_class_hard_reg_index[cl][i] >= 0)
 	    {
	      ira_hard_regno_allocno_class[i] = cl;
 	      break;
 	    }
 	}
#endif
    }
}



/* Form IRA_REG_CLASS_MAX_NREGS and IRA_REG_CLASS_MIN_NREGS maps.  */
static void
setup_reg_class_nregs (void)
{
  int i, cl, cl2, m;

  for (m = 0; m < MAX_MACHINE_MODE; m++)
    {
      for (cl = 0; cl < N_REG_CLASSES; cl++)
	ira_reg_class_max_nregs[cl][m]
	  = ira_reg_class_min_nregs[cl][m]
	  = targetm.class_max_nregs ((reg_class_t) cl, (enum machine_mode) m);
      for (cl = 0; cl < N_REG_CLASSES; cl++)
	for (i = 0;
	     (cl2 = alloc_reg_class_subclasses[cl][i]) != LIM_REG_CLASSES;
	     i++)
	  if (ira_reg_class_min_nregs[cl2][m]
	      < ira_reg_class_min_nregs[cl][m])
	    ira_reg_class_min_nregs[cl][m] = ira_reg_class_min_nregs[cl2][m];
    }
}



/* Set up IRA_PROHIBITED_CLASS_MODE_REGS.  */
static void
setup_prohibited_class_mode_regs (void)
{
  int j, k, hard_regno, cl;

  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    {
      for (j = 0; j < NUM_MACHINE_MODES; j++)
	{
	  CLEAR_HARD_REG_SET (ira_prohibited_class_mode_regs[cl][j]);
	  for (k = ira_class_hard_regs_num[cl] - 1; k >= 0; k--)
	    {
	      hard_regno = ira_class_hard_regs[cl][k];
	      if (! HARD_REGNO_MODE_OK (hard_regno, (enum machine_mode) j))
		SET_HARD_REG_BIT (ira_prohibited_class_mode_regs[cl][j],
				  hard_regno);
	    }
	}
    }
}

/* Clarify IRA_PROHIBITED_CLASS_MODE_REGS by excluding hard registers
   spanning from one register pressure class to another one.  It is
   called after defining the pressure classes.  */
static void
clarify_prohibited_class_mode_regs (void)
{
  int j, k, hard_regno, cl, pclass, nregs;

  for (cl = (int) N_REG_CLASSES - 1; cl >= 0; cl--)
    for (j = 0; j < NUM_MACHINE_MODES; j++)
      for (k = ira_class_hard_regs_num[cl] - 1; k >= 0; k--)
	{
	  hard_regno = ira_class_hard_regs[cl][k];
	  if (TEST_HARD_REG_BIT (ira_prohibited_class_mode_regs[cl][j], hard_regno))
	    continue;
	  nregs = hard_regno_nregs[hard_regno][j];
          if (hard_regno + nregs > FIRST_PSEUDO_REGISTER)
            {
              SET_HARD_REG_BIT (ira_prohibited_class_mode_regs[cl][j],
                                hard_regno);
               continue;
            }
	  pclass = ira_pressure_class_translate[REGNO_REG_CLASS (hard_regno)];
	  for (nregs-- ;nregs >= 0; nregs--)
	    if (((enum reg_class) pclass
		 != ira_pressure_class_translate[REGNO_REG_CLASS
						 (hard_regno + nregs)]))
	      {
		SET_HARD_REG_BIT (ira_prohibited_class_mode_regs[cl][j],
				  hard_regno);
		break;
	      }
	}
}



/* Allocate and initialize IRA_REGISTER_MOVE_COST,
   IRA_MAX_REGISTER_MOVE_COST, IRA_MAY_MOVE_IN_COST,
   IRA_MAY_MOVE_OUT_COST, IRA_MAX_MAY_MOVE_IN_COST, and
   IRA_MAX_MAY_MOVE_OUT_COST for MODE if it is not done yet.  */
void
ira_init_register_move_cost (enum machine_mode mode)
{
  int cl1, cl2, cl3;

  ira_assert (ira_register_move_cost[mode] == NULL
	      && ira_max_register_move_cost[mode] == NULL
	      && ira_may_move_in_cost[mode] == NULL
	      && ira_may_move_out_cost[mode] == NULL
	      && ira_max_may_move_in_cost[mode] == NULL
	      && ira_max_may_move_out_cost[mode] == NULL);
  if (move_cost[mode] == NULL)
    init_move_cost (mode);
  ira_register_move_cost[mode] = move_cost[mode];
  /* Don't use ira_allocate because the tables exist out of scope of a
     IRA call.  */
  ira_max_register_move_cost[mode]
    = (move_table *) xmalloc (sizeof (move_table) * N_REG_CLASSES);
  memcpy (ira_max_register_move_cost[mode], ira_register_move_cost[mode],
	  sizeof (move_table) * N_REG_CLASSES);
  for (cl1 = 0; cl1 < N_REG_CLASSES; cl1++)
    {
      /* Some subclasses are to small to have enough registers to hold
	 a value of MODE.  Just ignore them.  */
      if (ira_reg_class_max_nregs[cl1][mode] > ira_available_class_regs[cl1])
	continue;
      COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl1]);
      AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
      if (hard_reg_set_empty_p (temp_hard_regset))
	continue;
      for (cl2 = 0; cl2 < N_REG_CLASSES; cl2++)
	if (hard_reg_set_subset_p (reg_class_contents[cl1],
				   reg_class_contents[cl2]))
	  for (cl3 = 0; cl3 < N_REG_CLASSES; cl3++)
	    {
	      if (ira_max_register_move_cost[mode][cl2][cl3]
		  < ira_register_move_cost[mode][cl1][cl3])
		ira_max_register_move_cost[mode][cl2][cl3]
		  = ira_register_move_cost[mode][cl1][cl3];
	      if (ira_max_register_move_cost[mode][cl3][cl2]
		  < ira_register_move_cost[mode][cl3][cl1])
		ira_max_register_move_cost[mode][cl3][cl2]
		  = ira_register_move_cost[mode][cl3][cl1];
	    }
    }
  ira_may_move_in_cost[mode]
    = (move_table *) xmalloc (sizeof (move_table) * N_REG_CLASSES);
  memcpy (ira_may_move_in_cost[mode], may_move_in_cost[mode],
	  sizeof (move_table) * N_REG_CLASSES);
  ira_may_move_out_cost[mode]
    = (move_table *) xmalloc (sizeof (move_table) * N_REG_CLASSES);
  memcpy (ira_may_move_out_cost[mode], may_move_out_cost[mode],
	  sizeof (move_table) * N_REG_CLASSES);
  ira_max_may_move_in_cost[mode]
    = (move_table *) xmalloc (sizeof (move_table) * N_REG_CLASSES);
  memcpy (ira_max_may_move_in_cost[mode], ira_max_register_move_cost[mode],
	  sizeof (move_table) * N_REG_CLASSES);
  ira_max_may_move_out_cost[mode]
    = (move_table *) xmalloc (sizeof (move_table) * N_REG_CLASSES);
  memcpy (ira_max_may_move_out_cost[mode], ira_max_register_move_cost[mode],
	  sizeof (move_table) * N_REG_CLASSES);
  for (cl1 = 0; cl1 < N_REG_CLASSES; cl1++)
    {
      for (cl2 = 0; cl2 < N_REG_CLASSES; cl2++)
	{
	  COPY_HARD_REG_SET (temp_hard_regset, reg_class_contents[cl2]);
	  AND_COMPL_HARD_REG_SET (temp_hard_regset, no_unit_alloc_regs);
	  if (hard_reg_set_empty_p (temp_hard_regset))
	    continue;
	  if (ira_class_subset_p[cl1][cl2])
	    ira_may_move_in_cost[mode][cl1][cl2] = 0;
	  if (ira_class_subset_p[cl2][cl1])
	    ira_may_move_out_cost[mode][cl1][cl2] = 0;
	  if (ira_class_subset_p[cl1][cl2])
	    ira_max_may_move_in_cost[mode][cl1][cl2] = 0;
	  if (ira_class_subset_p[cl2][cl1])
	    ira_max_may_move_out_cost[mode][cl1][cl2] = 0;
	  ira_register_move_cost[mode][cl1][cl2]
	    = ira_max_register_move_cost[mode][cl1][cl2];
	  ira_may_move_in_cost[mode][cl1][cl2]
	    = ira_max_may_move_in_cost[mode][cl1][cl2];
	  ira_may_move_out_cost[mode][cl1][cl2]
	    = ira_max_may_move_out_cost[mode][cl1][cl2];
	}
    }
}



/* This is called once during compiler work.  It sets up
   different arrays whose values don't depend on the compiled
   function.  */
void
ira_init_once (void)
{
  int mode;

  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
    {
      ira_register_move_cost[mode] = NULL;
      ira_max_register_move_cost[mode] = NULL;
      ira_may_move_in_cost[mode] = NULL;
      ira_may_move_out_cost[mode] = NULL;
      ira_max_may_move_in_cost[mode] = NULL;
      ira_max_may_move_out_cost[mode] = NULL;
    }
  ira_init_costs_once ();
}

/* Free ira_max_register_move_cost, ira_may_move_in_cost,
   ira_may_move_out_cost, ira_max_may_move_in_cost, and
   ira_max_may_move_out_cost for each mode.  */
static void
free_register_move_costs (void)
{
  int mode;

  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
    {
      free (ira_max_register_move_cost[mode]);
      free (ira_may_move_in_cost[mode]);
      free (ira_may_move_out_cost[mode]);
      free (ira_max_may_move_in_cost[mode]);
      free (ira_max_may_move_out_cost[mode]);
      ira_register_move_cost[mode] = NULL;
      ira_max_register_move_cost[mode] = NULL;
      ira_may_move_in_cost[mode] = NULL;
      ira_may_move_out_cost[mode] = NULL;
      ira_max_may_move_in_cost[mode] = NULL;
      ira_max_may_move_out_cost[mode] = NULL;
    }
}

/* This is called every time when register related information is
   changed.  */
void
ira_init (void)
{
  free_register_move_costs ();
  setup_reg_mode_hard_regset ();
  setup_alloc_regs (flag_omit_frame_pointer != 0);
  setup_class_subset_and_memory_move_costs ();
  setup_reg_class_nregs ();
  setup_prohibited_class_mode_regs ();
  find_reg_classes ();
  clarify_prohibited_class_mode_regs ();
  setup_hard_regno_aclass ();
  ira_init_costs ();
}

/* Function called once at the end of compiler work.  */
void
ira_finish_once (void)
{
  ira_finish_costs_once ();
  free_register_move_costs ();
}


#define ira_prohibited_mode_move_regs_initialized_p \
  (this_target_ira_int->x_ira_prohibited_mode_move_regs_initialized_p)

/* Set up IRA_PROHIBITED_MODE_MOVE_REGS.  */
static void
setup_prohibited_mode_move_regs (void)
{
  int i, j;
  rtx test_reg1, test_reg2, move_pat, move_insn;

  if (ira_prohibited_mode_move_regs_initialized_p)
    return;
  ira_prohibited_mode_move_regs_initialized_p = true;
  test_reg1 = gen_rtx_REG (VOIDmode, 0);
  test_reg2 = gen_rtx_REG (VOIDmode, 0);
  move_pat = gen_rtx_SET (VOIDmode, test_reg1, test_reg2);
  move_insn = gen_rtx_INSN (VOIDmode, 0, 0, 0, 0, move_pat, 0, -1, 0);
  for (i = 0; i < NUM_MACHINE_MODES; i++)
    {
      SET_HARD_REG_SET (ira_prohibited_mode_move_regs[i]);
      for (j = 0; j < FIRST_PSEUDO_REGISTER; j++)
	{
	  if (! HARD_REGNO_MODE_OK (j, (enum machine_mode) i))
	    continue;
	  SET_REGNO_RAW (test_reg1, j);
	  PUT_MODE (test_reg1, (enum machine_mode) i);
	  SET_REGNO_RAW (test_reg2, j);
	  PUT_MODE (test_reg2, (enum machine_mode) i);
	  INSN_CODE (move_insn) = -1;
	  recog_memoized (move_insn);
	  if (INSN_CODE (move_insn) < 0)
	    continue;
	  extract_insn (move_insn);
	  if (! constrain_operands (1))
	    continue;
	  CLEAR_HARD_REG_BIT (ira_prohibited_mode_move_regs[i], j);
	}
    }
}



/* Return nonzero if REGNO is a particularly bad choice for reloading X.  */
static bool
ira_bad_reload_regno_1 (int regno, rtx x)
{
  int x_regno, n, i;
  ira_allocno_t a;
  enum reg_class pref;

  /* We only deal with pseudo regs.  */
  if (! x || GET_CODE (x) != REG)
    return false;

  x_regno = REGNO (x);
  if (x_regno < FIRST_PSEUDO_REGISTER)
    return false;

  /* If the pseudo prefers REGNO explicitly, then do not consider
     REGNO a bad spill choice.  */
  pref = reg_preferred_class (x_regno);
  if (reg_class_size[pref] == 1)
    return !TEST_HARD_REG_BIT (reg_class_contents[pref], regno);

  /* If the pseudo conflicts with REGNO, then we consider REGNO a
     poor choice for a reload regno.  */
  a = ira_regno_allocno_map[x_regno];
  n = ALLOCNO_NUM_OBJECTS (a);
  for (i = 0; i < n; i++)
    {
      ira_object_t obj = ALLOCNO_OBJECT (a, i);
      if (TEST_HARD_REG_BIT (OBJECT_TOTAL_CONFLICT_HARD_REGS (obj), regno))
	return true;
    }
  return false;
}

/* Return nonzero if REGNO is a particularly bad choice for reloading
   IN or OUT.  */
bool
ira_bad_reload_regno (int regno, rtx in, rtx out)
{
  return (ira_bad_reload_regno_1 (regno, in)
	  || ira_bad_reload_regno_1 (regno, out));
}

/* Return TRUE if *LOC contains an asm.  */
static int
insn_contains_asm_1 (rtx *loc, void *data ATTRIBUTE_UNUSED)
{
  if ( !*loc)
    return FALSE;
  if (GET_CODE (*loc) == ASM_OPERANDS)
    return TRUE;
  return FALSE;
}


/* Return TRUE if INSN contains an ASM.  */
static bool
insn_contains_asm (rtx insn)
{
  return for_each_rtx (&insn, insn_contains_asm_1, NULL);
}

/* Add register clobbers from asm statements.  */
static void
compute_regs_asm_clobbered (void)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    {
      rtx insn;
      FOR_BB_INSNS_REVERSE (bb, insn)
	{
	  df_ref *def_rec;

	  if (insn_contains_asm (insn))
	    for (def_rec = DF_INSN_DEFS (insn); *def_rec; def_rec++)
	      {
		df_ref def = *def_rec;
		unsigned int dregno = DF_REF_REGNO (def);
		if (HARD_REGISTER_NUM_P (dregno))
		  add_to_hard_reg_set (&crtl->asm_clobbers,
				       GET_MODE (DF_REF_REAL_REG (def)),
				       dregno);
	      }
	}
    }
}


/* Set up ELIMINABLE_REGSET, IRA_NO_ALLOC_REGS, and REGS_EVER_LIVE.  */
void
ira_setup_eliminable_regset (void)
{
#ifdef ELIMINABLE_REGS
  int i;
  static const struct {const int from, to; } eliminables[] = ELIMINABLE_REGS;
#endif
  /* FIXME: If EXIT_IGNORE_STACK is set, we will not save and restore
     sp for alloca.  So we can't eliminate the frame pointer in that
     case.  At some point, we should improve this by emitting the
     sp-adjusting insns for this case.  */
  int need_fp
    = (! flag_omit_frame_pointer
       || (cfun->calls_alloca && EXIT_IGNORE_STACK)
       /* We need the frame pointer to catch stack overflow exceptions
	  if the stack pointer is moving.  */
       || (flag_stack_check && STACK_CHECK_MOVING_SP)
       || crtl->accesses_prior_frames
       || crtl->stack_realign_needed
       || targetm.frame_pointer_required ());

  frame_pointer_needed = need_fp;

  COPY_HARD_REG_SET (ira_no_alloc_regs, no_unit_alloc_regs);
  CLEAR_HARD_REG_SET (eliminable_regset);

  compute_regs_asm_clobbered ();

  /* Build the regset of all eliminable registers and show we can't
     use those that we already know won't be eliminated.  */
#ifdef ELIMINABLE_REGS
  for (i = 0; i < (int) ARRAY_SIZE (eliminables); i++)
    {
      bool cannot_elim
	= (! targetm.can_eliminate (eliminables[i].from, eliminables[i].to)
	   || (eliminables[i].to == STACK_POINTER_REGNUM && need_fp));

      if (!TEST_HARD_REG_BIT (crtl->asm_clobbers, eliminables[i].from))
	{
	    SET_HARD_REG_BIT (eliminable_regset, eliminables[i].from);

	    if (cannot_elim)
	      SET_HARD_REG_BIT (ira_no_alloc_regs, eliminables[i].from);
	}
      else if (cannot_elim)
	error ("%s cannot be used in asm here",
	       reg_names[eliminables[i].from]);
      else
	df_set_regs_ever_live (eliminables[i].from, true);
    }
#if !HARD_FRAME_POINTER_IS_FRAME_POINTER
  if (!TEST_HARD_REG_BIT (crtl->asm_clobbers, HARD_FRAME_POINTER_REGNUM))
    {
      SET_HARD_REG_BIT (eliminable_regset, HARD_FRAME_POINTER_REGNUM);
      if (need_fp)
	SET_HARD_REG_BIT (ira_no_alloc_regs, HARD_FRAME_POINTER_REGNUM);
    }
  else if (need_fp)
    error ("%s cannot be used in asm here",
	   reg_names[HARD_FRAME_POINTER_REGNUM]);
  else
    df_set_regs_ever_live (HARD_FRAME_POINTER_REGNUM, true);
#endif

#else
  if (!TEST_HARD_REG_BIT (crtl->asm_clobbers, HARD_FRAME_POINTER_REGNUM))
    {
      SET_HARD_REG_BIT (eliminable_regset, FRAME_POINTER_REGNUM);
      if (need_fp)
	SET_HARD_REG_BIT (ira_no_alloc_regs, FRAME_POINTER_REGNUM);
    }
  else if (need_fp)
    error ("%s cannot be used in asm here", reg_names[FRAME_POINTER_REGNUM]);
  else
    df_set_regs_ever_live (FRAME_POINTER_REGNUM, true);
#endif
}



/* The length of the following two arrays.  */
int ira_reg_equiv_len;

/* The element value is TRUE if the corresponding regno value is
   invariant.  */
bool *ira_reg_equiv_invariant_p;

/* The element value is equiv constant of given pseudo-register or
   NULL_RTX.  */
rtx *ira_reg_equiv_const;

/* Set up the two arrays declared above.  */
static void
find_reg_equiv_invariant_const (void)
{
  unsigned int i;
  bool invariant_p;
  rtx list, insn, note, constant, x;

  for (i = FIRST_PSEUDO_REGISTER; i < VEC_length (reg_equivs_t, reg_equivs); i++)
    {
      constant = NULL_RTX;
      invariant_p = false;
      for (list = reg_equiv_init (i); list != NULL_RTX; list = XEXP (list, 1))
	{
	  insn = XEXP (list, 0);
	  note = find_reg_note (insn, REG_EQUIV, NULL_RTX);

	  if (note == NULL_RTX)
	    continue;

	  x = XEXP (note, 0);

	  if (! CONSTANT_P (x)
	      || ! flag_pic || LEGITIMATE_PIC_OPERAND_P (x))
	    {
	      /* It can happen that a REG_EQUIV note contains a MEM
		 that is not a legitimate memory operand.  As later
		 stages of the reload assume that all addresses found
		 in the reg_equiv_* arrays were originally legitimate,
		 we ignore such REG_EQUIV notes.  */
	      if (memory_operand (x, VOIDmode))
		invariant_p = MEM_READONLY_P (x);
	      else if (function_invariant_p (x))
		{
		  if (GET_CODE (x) == PLUS
		      || x == frame_pointer_rtx || x == arg_pointer_rtx)
		    invariant_p = true;
		  else
		    constant = x;
		}
	    }
	}
      ira_reg_equiv_invariant_p[i] = invariant_p;
      ira_reg_equiv_const[i] = constant;
    }
}



/* Vector of substitutions of register numbers,
   used to map pseudo regs into hardware regs.
   This is set up as a result of register allocation.
   Element N is the hard reg assigned to pseudo reg N,
   or is -1 if no hard reg was assigned.
   If N is a hard reg number, element N is N.  */
short *reg_renumber;

/* Set up REG_RENUMBER and CALLER_SAVE_NEEDED (used by reload) from
   the allocation found by IRA.  */
static void
setup_reg_renumber (void)
{
  int regno, hard_regno;
  ira_allocno_t a;
  ira_allocno_iterator ai;

  caller_save_needed = 0;
  FOR_EACH_ALLOCNO (a, ai)
    {
      /* There are no caps at this point.  */
      ira_assert (ALLOCNO_CAP_MEMBER (a) == NULL);
      if (! ALLOCNO_ASSIGNED_P (a))
	/* It can happen if A is not referenced but partially anticipated
	   somewhere in a region.  */
	ALLOCNO_ASSIGNED_P (a) = true;
      ira_free_allocno_updated_costs (a);
      hard_regno = ALLOCNO_HARD_REGNO (a);
      regno = ALLOCNO_REGNO (a);
      reg_renumber[regno] = (hard_regno < 0 ? -1 : hard_regno);
      if (hard_regno >= 0)
	{
	  int i, nwords;
	  enum reg_class pclass;
	  ira_object_t obj;
	  
	  pclass = ira_pressure_class_translate[REGNO_REG_CLASS (hard_regno)];
	  nwords = ALLOCNO_NUM_OBJECTS (a);
	  for (i = 0; i < nwords; i++)
	    {
	      obj = ALLOCNO_OBJECT (a, i);
	      IOR_COMPL_HARD_REG_SET (OBJECT_TOTAL_CONFLICT_HARD_REGS (obj),
				      reg_class_contents[pclass]);
	    }
	  if (ALLOCNO_CALLS_CROSSED_NUM (a) != 0
	      && ira_hard_reg_set_intersection_p (hard_regno, ALLOCNO_MODE (a),
						  call_used_reg_set))
	    {
	      ira_assert (!optimize || flag_caller_saves
			  || regno >= ira_reg_equiv_len
			  || ira_reg_equiv_const[regno]
			  || ira_reg_equiv_invariant_p[regno]);
	      caller_save_needed = 1;
	    }
	}
    }
}

/* Set up allocno assignment flags for further allocation
   improvements.  */
static void
setup_allocno_assignment_flags (void)
{
  int hard_regno;
  ira_allocno_t a;
  ira_allocno_iterator ai;

  FOR_EACH_ALLOCNO (a, ai)
    {
      if (! ALLOCNO_ASSIGNED_P (a))
	/* It can happen if A is not referenced but partially anticipated
	   somewhere in a region.  */
	ira_free_allocno_updated_costs (a);
      hard_regno = ALLOCNO_HARD_REGNO (a);
      /* Don't assign hard registers to allocnos which are destination
	 of removed store at the end of loop.  It has no sense to keep
	 the same value in different hard registers.  It is also
	 impossible to assign hard registers correctly to such
	 allocnos because the cost info and info about intersected
	 calls are incorrect for them.  */
      ALLOCNO_ASSIGNED_P (a) = (hard_regno >= 0
				|| ALLOCNO_EMIT_DATA (a)->mem_optimized_dest_p
				|| (ALLOCNO_MEMORY_COST (a)
				    - ALLOCNO_CLASS_COST (a)) < 0);
      ira_assert
	(hard_regno < 0
	 || ira_hard_reg_in_set_p (hard_regno, ALLOCNO_MODE (a),
				   reg_class_contents[ALLOCNO_CLASS (a)]));
    }
}

/* Evaluate overall allocation cost and the costs for using hard
   registers and memory for allocnos.  */
static void
calculate_allocation_cost (void)
{
  int hard_regno, cost;
  ira_allocno_t a;
  ira_allocno_iterator ai;

  ira_overall_cost = ira_reg_cost = ira_mem_cost = 0;
  FOR_EACH_ALLOCNO (a, ai)
    {
      hard_regno = ALLOCNO_HARD_REGNO (a);
      ira_assert (hard_regno < 0
		  || (ira_hard_reg_in_set_p
		      (hard_regno, ALLOCNO_MODE (a),
		       reg_class_contents[ALLOCNO_CLASS (a)])));
      if (hard_regno < 0)
	{
	  cost = ALLOCNO_MEMORY_COST (a);
	  ira_mem_cost += cost;
	}
      else if (ALLOCNO_HARD_REG_COSTS (a) != NULL)
	{
	  cost = (ALLOCNO_HARD_REG_COSTS (a)
		  [ira_class_hard_reg_index
		   [ALLOCNO_CLASS (a)][hard_regno]]);
	  ira_reg_cost += cost;
	}
      else
	{
	  cost = ALLOCNO_CLASS_COST (a);
	  ira_reg_cost += cost;
	}
      ira_overall_cost += cost;
    }

  if (internal_flag_ira_verbose > 0 && ira_dump_file != NULL)
    {
      fprintf (ira_dump_file,
	       "+++Costs: overall %d, reg %d, mem %d, ld %d, st %d, move %d\n",
	       ira_overall_cost, ira_reg_cost, ira_mem_cost,
	       ira_load_cost, ira_store_cost, ira_shuffle_cost);
      fprintf (ira_dump_file, "+++       move loops %d, new jumps %d\n",
	       ira_move_loops_num, ira_additional_jumps_num);
    }

}

#ifdef ENABLE_IRA_CHECKING
/* Check the correctness of the allocation.  We do need this because
   of complicated code to transform more one region internal
   representation into one region representation.  */
static void
check_allocation (void)
{
  ira_allocno_t a;
  int hard_regno, nregs, conflict_nregs;
  ira_allocno_iterator ai;

  FOR_EACH_ALLOCNO (a, ai)
    {
      int n = ALLOCNO_NUM_OBJECTS (a);
      int i;

      if (ALLOCNO_CAP_MEMBER (a) != NULL
	  || (hard_regno = ALLOCNO_HARD_REGNO (a)) < 0)
	continue;
      nregs = hard_regno_nregs[hard_regno][ALLOCNO_MODE (a)];
      if (nregs == 1)
	/* We allocated a single hard register.  */
	n = 1;
      else if (n > 1)
	/* We allocated multiple hard registers, and we will test
	   conflicts in a granularity of single hard regs.  */
	nregs = 1;

      for (i = 0; i < n; i++)
	{
	  ira_object_t obj = ALLOCNO_OBJECT (a, i);
	  ira_object_t conflict_obj;
	  ira_object_conflict_iterator oci;
	  int this_regno = hard_regno;
	  if (n > 1)
	    {
	      if (WORDS_BIG_ENDIAN)
		this_regno += n - i - 1;
	      else
		this_regno += i;
	    }
	  FOR_EACH_OBJECT_CONFLICT (obj, conflict_obj, oci)
	    {
	      ira_allocno_t conflict_a = OBJECT_ALLOCNO (conflict_obj);
	      int conflict_hard_regno = ALLOCNO_HARD_REGNO (conflict_a);
	      if (conflict_hard_regno < 0)
		continue;

	      conflict_nregs
		= (hard_regno_nregs
		   [conflict_hard_regno][ALLOCNO_MODE (conflict_a)]);

	      if (ALLOCNO_NUM_OBJECTS (conflict_a) > 1
		  && conflict_nregs == ALLOCNO_NUM_OBJECTS (conflict_a))
		{
		  if (WORDS_BIG_ENDIAN)
		    conflict_hard_regno += (ALLOCNO_NUM_OBJECTS (conflict_a)
					    - OBJECT_SUBWORD (conflict_obj) - 1);
		  else
		    conflict_hard_regno += OBJECT_SUBWORD (conflict_obj);
		  conflict_nregs = 1;
		}

	      if ((conflict_hard_regno <= this_regno
		 && this_regno < conflict_hard_regno + conflict_nregs)
		|| (this_regno <= conflict_hard_regno
		    && conflict_hard_regno < this_regno + nregs))
		{
		  fprintf (stderr, "bad allocation for %d and %d\n",
			   ALLOCNO_REGNO (a), ALLOCNO_REGNO (conflict_a));
		  gcc_unreachable ();
		}
	    }
	}
    }
}
#endif

/* Fix values of array REG_EQUIV_INIT after live range splitting done
   by IRA.  */
static void
fix_reg_equiv_init (void)
{
  unsigned int max_regno = max_reg_num ();
  int i, new_regno, max;
  rtx x, prev, next, insn, set;

  if (VEC_length (reg_equivs_t, reg_equivs) < max_regno)
    {
      max = VEC_length (reg_equivs_t, reg_equivs);
      grow_reg_equivs ();
      for (i = FIRST_PSEUDO_REGISTER; i < max; i++)
	for (prev = NULL_RTX, x = reg_equiv_init (i);
	     x != NULL_RTX;
	     x = next)
	  {
	    next = XEXP (x, 1);
	    insn = XEXP (x, 0);
	    set = single_set (insn);
	    ira_assert (set != NULL_RTX
			&& (REG_P (SET_DEST (set)) || REG_P (SET_SRC (set))));
	    if (REG_P (SET_DEST (set))
		&& ((int) REGNO (SET_DEST (set)) == i
		    || (int) ORIGINAL_REGNO (SET_DEST (set)) == i))
	      new_regno = REGNO (SET_DEST (set));
	    else if (REG_P (SET_SRC (set))
		     && ((int) REGNO (SET_SRC (set)) == i
			 || (int) ORIGINAL_REGNO (SET_SRC (set)) == i))
	      new_regno = REGNO (SET_SRC (set));
	    else
 	      gcc_unreachable ();
	    if (new_regno == i)
	      prev = x;
	    else
	      {
		if (prev == NULL_RTX)
		  reg_equiv_init (i) = next;
		else
		  XEXP (prev, 1) = next;
		XEXP (x, 1) = reg_equiv_init (new_regno);
		reg_equiv_init (new_regno) = x;
	      }
	  }
    }
}

#ifdef ENABLE_IRA_CHECKING
/* Print redundant memory-memory copies.  */
static void
print_redundant_copies (void)
{
  int hard_regno;
  ira_allocno_t a;
  ira_copy_t cp, next_cp;
  ira_allocno_iterator ai;

  FOR_EACH_ALLOCNO (a, ai)
    {
      if (ALLOCNO_CAP_MEMBER (a) != NULL)
	/* It is a cap. */
	continue;
      hard_regno = ALLOCNO_HARD_REGNO (a);
      if (hard_regno >= 0)
	continue;
      for (cp = ALLOCNO_COPIES (a); cp != NULL; cp = next_cp)
	if (cp->first == a)
	  next_cp = cp->next_first_allocno_copy;
	else
	  {
	    next_cp = cp->next_second_allocno_copy;
	    if (internal_flag_ira_verbose > 4 && ira_dump_file != NULL
		&& cp->insn != NULL_RTX
		&& ALLOCNO_HARD_REGNO (cp->first) == hard_regno)
	      fprintf (ira_dump_file,
		       "        Redundant move from %d(freq %d):%d\n",
		       INSN_UID (cp->insn), cp->freq, hard_regno);
	  }
    }
}
#endif

/* Setup preferred and alternative classes for new pseudo-registers
   created by IRA starting with START.  */
static void
setup_preferred_alternate_classes_for_new_pseudos (int start)
{
  int i, old_regno;
  int max_regno = max_reg_num ();

  for (i = start; i < max_regno; i++)
    {
      old_regno = ORIGINAL_REGNO (regno_reg_rtx[i]);
      ira_assert (i != old_regno);
      setup_reg_classes (i, reg_preferred_class (old_regno),
			 reg_alternate_class (old_regno),
			 reg_allocno_class (old_regno));
      if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	fprintf (ira_dump_file,
		 "    New r%d: setting preferred %s, alternative %s\n",
		 i, reg_class_names[reg_preferred_class (old_regno)],
		 reg_class_names[reg_alternate_class (old_regno)]);
    }
}



/* Regional allocation can create new pseudo-registers.  This function
   expands some arrays for pseudo-registers.  */
static void
expand_reg_info (int old_size)
{
  int i;
  int size = max_reg_num ();

  resize_reg_info ();
  for (i = old_size; i < size; i++)
    setup_reg_classes (i, GENERAL_REGS, ALL_REGS, GENERAL_REGS);
}

/* Return TRUE if there is too high register pressure in the function.
   It is used to decide when stack slot sharing is worth to do.  */
static bool
too_high_register_pressure_p (void)
{
  int i;
  enum reg_class pclass;

  for (i = 0; i < ira_pressure_classes_num; i++)
    {
      pclass = ira_pressure_classes[i];
      if (ira_loop_tree_root->reg_pressure[pclass] > 10000)
	return true;
    }
  return false;
}



/* Indicate that hard register number FROM was eliminated and replaced with
   an offset from hard register number TO.  The status of hard registers live
   at the start of a basic block is updated by replacing a use of FROM with
   a use of TO.  */

void
mark_elimination (int from, int to)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    {
      /* We don't use LIVE info in IRA.  */
      bitmap r = DF_LR_IN (bb);

      if (REGNO_REG_SET_P (r, from))
	{
	  CLEAR_REGNO_REG_SET (r, from);
	  SET_REGNO_REG_SET (r, to);
	}
    }
}



struct equivalence
{
  /* Set when a REG_EQUIV note is found or created.  Use to
     keep track of what memory accesses might be created later,
     e.g. by reload.  */
  rtx replacement;
  rtx *src_p;
  /* The list of each instruction which initializes this register.  */
  rtx init_insns;
  /* Loop depth is used to recognize equivalences which appear
     to be present within the same loop (or in an inner loop).  */
  int loop_depth;
  /* Nonzero if this had a preexisting REG_EQUIV note.  */
  int is_arg_equivalence;
  /* Set when an attempt should be made to replace a register
     with the associated src_p entry.  */
  char replace;
};

/* reg_equiv[N] (where N is a pseudo reg number) is the equivalence
   structure for that register.  */
static struct equivalence *reg_equiv;

/* Used for communication between the following two functions: contains
   a MEM that we wish to ensure remains unchanged.  */
static rtx equiv_mem;

/* Set nonzero if EQUIV_MEM is modified.  */
static int equiv_mem_modified;

/* If EQUIV_MEM is modified by modifying DEST, indicate that it is modified.
   Called via note_stores.  */
static void
validate_equiv_mem_from_store (rtx dest, const_rtx set ATTRIBUTE_UNUSED,
			       void *data ATTRIBUTE_UNUSED)
{
  if ((REG_P (dest)
       && reg_overlap_mentioned_p (dest, equiv_mem))
      || (MEM_P (dest)
	  && true_dependence (dest, VOIDmode, equiv_mem, rtx_varies_p)))
    equiv_mem_modified = 1;
}

/* Verify that no store between START and the death of REG invalidates
   MEMREF.  MEMREF is invalidated by modifying a register used in MEMREF,
   by storing into an overlapping memory location, or with a non-const
   CALL_INSN.

   Return 1 if MEMREF remains valid.  */
static int
validate_equiv_mem (rtx start, rtx reg, rtx memref)
{
  rtx insn;
  rtx note;

  equiv_mem = memref;
  equiv_mem_modified = 0;

  /* If the memory reference has side effects or is volatile, it isn't a
     valid equivalence.  */
  if (side_effects_p (memref))
    return 0;

  for (insn = start; insn && ! equiv_mem_modified; insn = NEXT_INSN (insn))
    {
      if (! INSN_P (insn))
	continue;

      if (find_reg_note (insn, REG_DEAD, reg))
	return 1;

      /* This used to ignore readonly memory and const/pure calls.  The problem
	 is the equivalent form may reference a pseudo which gets assigned a
	 call clobbered hard reg.  When we later replace REG with its
	 equivalent form, the value in the call-clobbered reg has been
	 changed and all hell breaks loose.  */
      if (CALL_P (insn))
	return 0;

      note_stores (PATTERN (insn), validate_equiv_mem_from_store, NULL);

      /* If a register mentioned in MEMREF is modified via an
	 auto-increment, we lose the equivalence.  Do the same if one
	 dies; although we could extend the life, it doesn't seem worth
	 the trouble.  */

      for (note = REG_NOTES (insn); note; note = XEXP (note, 1))
	if ((REG_NOTE_KIND (note) == REG_INC
	     || REG_NOTE_KIND (note) == REG_DEAD)
	    && REG_P (XEXP (note, 0))
	    && reg_overlap_mentioned_p (XEXP (note, 0), memref))
	  return 0;
    }

  return 0;
}

/* Returns zero if X is known to be invariant.  */
static int
equiv_init_varies_p (rtx x)
{
  RTX_CODE code = GET_CODE (x);
  int i;
  const char *fmt;

  switch (code)
    {
    case MEM:
      return !MEM_READONLY_P (x) || equiv_init_varies_p (XEXP (x, 0));

    case CONST:
    case CONST_INT:
    case CONST_DOUBLE:
    case CONST_FIXED:
    case CONST_VECTOR:
    case SYMBOL_REF:
    case LABEL_REF:
      return 0;

    case REG:
      return reg_equiv[REGNO (x)].replace == 0 && rtx_varies_p (x, 0);

    case ASM_OPERANDS:
      if (MEM_VOLATILE_P (x))
	return 1;

      /* Fall through.  */

    default:
      break;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    if (fmt[i] == 'e')
      {
	if (equiv_init_varies_p (XEXP (x, i)))
	  return 1;
      }
    else if (fmt[i] == 'E')
      {
	int j;
	for (j = 0; j < XVECLEN (x, i); j++)
	  if (equiv_init_varies_p (XVECEXP (x, i, j)))
	    return 1;
      }

  return 0;
}

/* Returns nonzero if X (used to initialize register REGNO) is movable.
   X is only movable if the registers it uses have equivalent initializations
   which appear to be within the same loop (or in an inner loop) and movable
   or if they are not candidates for local_alloc and don't vary.  */
static int
equiv_init_movable_p (rtx x, int regno)
{
  int i, j;
  const char *fmt;
  enum rtx_code code = GET_CODE (x);

  switch (code)
    {
    case SET:
      return equiv_init_movable_p (SET_SRC (x), regno);

    case CC0:
    case CLOBBER:
      return 0;

    case PRE_INC:
    case PRE_DEC:
    case POST_INC:
    case POST_DEC:
    case PRE_MODIFY:
    case POST_MODIFY:
      return 0;

    case REG:
      return ((reg_equiv[REGNO (x)].loop_depth >= reg_equiv[regno].loop_depth
	       && reg_equiv[REGNO (x)].replace)
	      || (REG_BASIC_BLOCK (REGNO (x)) < NUM_FIXED_BLOCKS
		  && ! rtx_varies_p (x, 0)));

    case UNSPEC_VOLATILE:
      return 0;

    case ASM_OPERANDS:
      if (MEM_VOLATILE_P (x))
	return 0;

      /* Fall through.  */

    default:
      break;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    switch (fmt[i])
      {
      case 'e':
	if (! equiv_init_movable_p (XEXP (x, i), regno))
	  return 0;
	break;
      case 'E':
	for (j = XVECLEN (x, i) - 1; j >= 0; j--)
	  if (! equiv_init_movable_p (XVECEXP (x, i, j), regno))
	    return 0;
	break;
      }

  return 1;
}

/* TRUE if X uses any registers for which reg_equiv[REGNO].replace is
   true.  */
static int
contains_replace_regs (rtx x)
{
  int i, j;
  const char *fmt;
  enum rtx_code code = GET_CODE (x);

  switch (code)
    {
    case CONST_INT:
    case CONST:
    case LABEL_REF:
    case SYMBOL_REF:
    case CONST_DOUBLE:
    case CONST_FIXED:
    case CONST_VECTOR:
    case PC:
    case CC0:
    case HIGH:
      return 0;

    case REG:
      return reg_equiv[REGNO (x)].replace;

    default:
      break;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    switch (fmt[i])
      {
      case 'e':
	if (contains_replace_regs (XEXP (x, i)))
	  return 1;
	break;
      case 'E':
	for (j = XVECLEN (x, i) - 1; j >= 0; j--)
	  if (contains_replace_regs (XVECEXP (x, i, j)))
	    return 1;
	break;
      }

  return 0;
}

/* TRUE if X references a memory location that would be affected by a store
   to MEMREF.  */
static int
memref_referenced_p (rtx memref, rtx x)
{
  int i, j;
  const char *fmt;
  enum rtx_code code = GET_CODE (x);

  switch (code)
    {
    case CONST_INT:
    case CONST:
    case LABEL_REF:
    case SYMBOL_REF:
    case CONST_DOUBLE:
    case CONST_FIXED:
    case CONST_VECTOR:
    case PC:
    case CC0:
    case HIGH:
    case LO_SUM:
      return 0;

    case REG:
      return (reg_equiv[REGNO (x)].replacement
	      && memref_referenced_p (memref,
				      reg_equiv[REGNO (x)].replacement));

    case MEM:
      if (true_dependence (memref, VOIDmode, x, rtx_varies_p))
	return 1;
      break;

    case SET:
      /* If we are setting a MEM, it doesn't count (its address does), but any
	 other SET_DEST that has a MEM in it is referencing the MEM.  */
      if (MEM_P (SET_DEST (x)))
	{
	  if (memref_referenced_p (memref, XEXP (SET_DEST (x), 0)))
	    return 1;
	}
      else if (memref_referenced_p (memref, SET_DEST (x)))
	return 1;

      return memref_referenced_p (memref, SET_SRC (x));

    default:
      break;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    switch (fmt[i])
      {
      case 'e':
	if (memref_referenced_p (memref, XEXP (x, i)))
	  return 1;
	break;
      case 'E':
	for (j = XVECLEN (x, i) - 1; j >= 0; j--)
	  if (memref_referenced_p (memref, XVECEXP (x, i, j)))
	    return 1;
	break;
      }

  return 0;
}

/* TRUE if some insn in the range (START, END] references a memory location
   that would be affected by a store to MEMREF.  */
static int
memref_used_between_p (rtx memref, rtx start, rtx end)
{
  rtx insn;

  for (insn = NEXT_INSN (start); insn != NEXT_INSN (end);
       insn = NEXT_INSN (insn))
    {
      if (!NONDEBUG_INSN_P (insn))
	continue;

      if (memref_referenced_p (memref, PATTERN (insn)))
	return 1;

      /* Nonconst functions may access memory.  */
      if (CALL_P (insn) && (! RTL_CONST_CALL_P (insn)))
	return 1;
    }

  return 0;
}

/* Mark REG as having no known equivalence.
   Some instructions might have been processed before and furnished
   with REG_EQUIV notes for this register; these notes will have to be
   removed.
   STORE is the piece of RTL that does the non-constant / conflicting
   assignment - a SET, CLOBBER or REG_INC note.  It is currently not used,
   but needs to be there because this function is called from note_stores.  */
static void
no_equiv (rtx reg, const_rtx store ATTRIBUTE_UNUSED,
	  void *data ATTRIBUTE_UNUSED)
{
  int regno;
  rtx list;

  if (!REG_P (reg))
    return;
  regno = REGNO (reg);
  list = reg_equiv[regno].init_insns;
  if (list == const0_rtx)
    return;
  reg_equiv[regno].init_insns = const0_rtx;
  reg_equiv[regno].replacement = NULL_RTX;
  /* This doesn't matter for equivalences made for argument registers, we
     should keep their initialization insns.  */
  if (reg_equiv[regno].is_arg_equivalence)
    return;
  reg_equiv_init (regno) = NULL_RTX;
  for (; list; list =  XEXP (list, 1))
    {
      rtx insn = XEXP (list, 0);
      remove_note (insn, find_reg_note (insn, REG_EQUIV, NULL_RTX));
    }
}

/* In DEBUG_INSN location adjust REGs from CLEARED_REGS bitmap to the
   equivalent replacement.  */

static rtx
adjust_cleared_regs (rtx loc, const_rtx old_rtx ATTRIBUTE_UNUSED, void *data)
{
  if (REG_P (loc))
    {
      bitmap cleared_regs = (bitmap) data;
      if (bitmap_bit_p (cleared_regs, REGNO (loc)))
	return simplify_replace_fn_rtx (*reg_equiv[REGNO (loc)].src_p,
					NULL_RTX, adjust_cleared_regs, data);
    }
  return NULL_RTX;
}

/* Nonzero if we recorded an equivalence for a LABEL_REF.  */
static int recorded_label_ref;

/* Find registers that are equivalent to a single value throughout the
   compilation (either because they can be referenced in memory or are
   set once from a single constant).  Lower their priority for a
   register.

   If such a register is only referenced once, try substituting its
   value into the using insn.  If it succeeds, we can eliminate the
   register completely.

   Initialize the REG_EQUIV_INIT array of initializing insns.

   Return non-zero if jump label rebuilding should be done.  */
static int
update_equiv_regs (void)
{
  rtx insn;
  basic_block bb;
  int loop_depth;
  bitmap cleared_regs;

  /* We need to keep track of whether or not we recorded a LABEL_REF so
     that we know if the jump optimizer needs to be rerun.  */
  recorded_label_ref = 0;

  reg_equiv = XCNEWVEC (struct equivalence, max_regno);
  grow_reg_equivs ();

  init_alias_analysis ();

  /* Scan the insns and find which registers have equivalences.  Do this
     in a separate scan of the insns because (due to -fcse-follow-jumps)
     a register can be set below its use.  */
  FOR_EACH_BB (bb)
    {
      loop_depth = bb->loop_depth;

      for (insn = BB_HEAD (bb);
	   insn != NEXT_INSN (BB_END (bb));
	   insn = NEXT_INSN (insn))
	{
	  rtx note;
	  rtx set;
	  rtx dest, src;
	  int regno;

	  if (! INSN_P (insn))
	    continue;

	  for (note = REG_NOTES (insn); note; note = XEXP (note, 1))
	    if (REG_NOTE_KIND (note) == REG_INC)
	      no_equiv (XEXP (note, 0), note, NULL);

	  set = single_set (insn);

	  /* If this insn contains more (or less) than a single SET,
	     only mark all destinations as having no known equivalence.  */
	  if (set == 0)
	    {
	      note_stores (PATTERN (insn), no_equiv, NULL);
	      continue;
	    }
	  else if (GET_CODE (PATTERN (insn)) == PARALLEL)
	    {
	      int i;

	      for (i = XVECLEN (PATTERN (insn), 0) - 1; i >= 0; i--)
		{
		  rtx part = XVECEXP (PATTERN (insn), 0, i);
		  if (part != set)
		    note_stores (part, no_equiv, NULL);
		}
	    }

	  dest = SET_DEST (set);
	  src = SET_SRC (set);

	  /* See if this is setting up the equivalence between an argument
	     register and its stack slot.  */
	  note = find_reg_note (insn, REG_EQUIV, NULL_RTX);
	  if (note)
	    {
	      gcc_assert (REG_P (dest));
	      regno = REGNO (dest);

	      /* Note that we don't want to clear reg_equiv_init even if there
		 are multiple sets of this register.  */
	      reg_equiv[regno].is_arg_equivalence = 1;

	      /* Record for reload that this is an equivalencing insn.  */
	      if (rtx_equal_p (src, XEXP (note, 0)))
		reg_equiv_init (regno)
		  = gen_rtx_INSN_LIST (VOIDmode, insn, reg_equiv_init (regno));

	      /* Continue normally in case this is a candidate for
		 replacements.  */
	    }

	  if (!optimize)
	    continue;

	  /* We only handle the case of a pseudo register being set
	     once, or always to the same value.  */
	  /* ??? The mn10200 port breaks if we add equivalences for
	     values that need an ADDRESS_REGS register and set them equivalent
	     to a MEM of a pseudo.  The actual problem is in the over-conservative
	     handling of INPADDR_ADDRESS / INPUT_ADDRESS / INPUT triples in
	     calculate_needs, but we traditionally work around this problem
	     here by rejecting equivalences when the destination is in a register
	     that's likely spilled.  This is fragile, of course, since the
	     preferred class of a pseudo depends on all instructions that set
	     or use it.  */

	  if (!REG_P (dest)
	      || (regno = REGNO (dest)) < FIRST_PSEUDO_REGISTER
	      || reg_equiv[regno].init_insns == const0_rtx
	      || (targetm.class_likely_spilled_p (reg_preferred_class (regno))
		  && MEM_P (src) && ! reg_equiv[regno].is_arg_equivalence))
	    {
	      /* This might be setting a SUBREG of a pseudo, a pseudo that is
		 also set somewhere else to a constant.  */
	      note_stores (set, no_equiv, NULL);
	      continue;
	    }

	  note = find_reg_note (insn, REG_EQUAL, NULL_RTX);

	  /* cse sometimes generates function invariants, but doesn't put a
	     REG_EQUAL note on the insn.  Since this note would be redundant,
	     there's no point creating it earlier than here.  */
	  if (! note && ! rtx_varies_p (src, 0))
	    note = set_unique_reg_note (insn, REG_EQUAL, copy_rtx (src));

	  /* Don't bother considering a REG_EQUAL note containing an EXPR_LIST
	     since it represents a function call */
	  if (note && GET_CODE (XEXP (note, 0)) == EXPR_LIST)
	    note = NULL_RTX;

	  if (DF_REG_DEF_COUNT (regno) != 1
	      && (! note
		  || rtx_varies_p (XEXP (note, 0), 0)
		  || (reg_equiv[regno].replacement
		      && ! rtx_equal_p (XEXP (note, 0),
					reg_equiv[regno].replacement))))
	    {
	      no_equiv (dest, set, NULL);
	      continue;
	    }
	  /* Record this insn as initializing this register.  */
	  reg_equiv[regno].init_insns
	    = gen_rtx_INSN_LIST (VOIDmode, insn, reg_equiv[regno].init_insns);

	  /* If this register is known to be equal to a constant, record that
	     it is always equivalent to the constant.  */
	  if (DF_REG_DEF_COUNT (regno) == 1
	      && note && ! rtx_varies_p (XEXP (note, 0), 0))
	    {
	      rtx note_value = XEXP (note, 0);
	      remove_note (insn, note);
	      set_unique_reg_note (insn, REG_EQUIV, note_value);
	    }

	  /* If this insn introduces a "constant" register, decrease the priority
	     of that register.  Record this insn if the register is only used once
	     more and the equivalence value is the same as our source.

	     The latter condition is checked for two reasons:  First, it is an
	     indication that it may be more efficient to actually emit the insn
	     as written (if no registers are available, reload will substitute
	     the equivalence).  Secondly, it avoids problems with any registers
	     dying in this insn whose death notes would be missed.

	     If we don't have a REG_EQUIV note, see if this insn is loading
	     a register used only in one basic block from a MEM.  If so, and the
	     MEM remains unchanged for the life of the register, add a REG_EQUIV
	     note.  */

	  note = find_reg_note (insn, REG_EQUIV, NULL_RTX);

	  if (note == 0 && REG_BASIC_BLOCK (regno) >= NUM_FIXED_BLOCKS
	      && MEM_P (SET_SRC (set))
	      && validate_equiv_mem (insn, dest, SET_SRC (set)))
	    note = set_unique_reg_note (insn, REG_EQUIV, copy_rtx (SET_SRC (set)));

	  if (note)
	    {
	      int regno = REGNO (dest);
	      rtx x = XEXP (note, 0);

	      /* If we haven't done so, record for reload that this is an
		 equivalencing insn.  */
	      if (!reg_equiv[regno].is_arg_equivalence)
		reg_equiv_init (regno)
		  = gen_rtx_INSN_LIST (VOIDmode, insn, reg_equiv_init (regno));

	      /* Record whether or not we created a REG_EQUIV note for a LABEL_REF.
		 We might end up substituting the LABEL_REF for uses of the
		 pseudo here or later.  That kind of transformation may turn an
		 indirect jump into a direct jump, in which case we must rerun the
		 jump optimizer to ensure that the JUMP_LABEL fields are valid.  */
	      if (GET_CODE (x) == LABEL_REF
		  || (GET_CODE (x) == CONST
		      && GET_CODE (XEXP (x, 0)) == PLUS
		      && (GET_CODE (XEXP (XEXP (x, 0), 0)) == LABEL_REF)))
		recorded_label_ref = 1;

	      reg_equiv[regno].replacement = x;
	      reg_equiv[regno].src_p = &SET_SRC (set);
	      reg_equiv[regno].loop_depth = loop_depth;

	      /* Don't mess with things live during setjmp.  */
	      if (REG_LIVE_LENGTH (regno) >= 0 && optimize)
		{
		  /* Note that the statement below does not affect the priority
		     in local-alloc!  */
		  REG_LIVE_LENGTH (regno) *= 2;

		  /* If the register is referenced exactly twice, meaning it is
		     set once and used once, indicate that the reference may be
		     replaced by the equivalence we computed above.  Do this
		     even if the register is only used in one block so that
		     dependencies can be handled where the last register is
		     used in a different block (i.e. HIGH / LO_SUM sequences)
		     and to reduce the number of registers alive across
		     calls.  */

		  if (REG_N_REFS (regno) == 2
		      && (rtx_equal_p (x, src)
			  || ! equiv_init_varies_p (src))
		      && NONJUMP_INSN_P (insn)
		      && equiv_init_movable_p (PATTERN (insn), regno))
		    reg_equiv[regno].replace = 1;
		}
	    }
	}
    }

  if (!optimize)
    goto out;

  /* A second pass, to gather additional equivalences with memory.  This needs
     to be done after we know which registers we are going to replace.  */

  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
    {
      rtx set, src, dest;
      unsigned regno;

      if (! INSN_P (insn))
	continue;

      set = single_set (insn);
      if (! set)
	continue;

      dest = SET_DEST (set);
      src = SET_SRC (set);

      /* If this sets a MEM to the contents of a REG that is only used
	 in a single basic block, see if the register is always equivalent
	 to that memory location and if moving the store from INSN to the
	 insn that set REG is safe.  If so, put a REG_EQUIV note on the
	 initializing insn.

	 Don't add a REG_EQUIV note if the insn already has one.  The existing
	 REG_EQUIV is likely more useful than the one we are adding.

	 If one of the regs in the address has reg_equiv[REGNO].replace set,
	 then we can't add this REG_EQUIV note.  The reg_equiv[REGNO].replace
	 optimization may move the set of this register immediately before
	 insn, which puts it after reg_equiv[REGNO].init_insns, and hence
	 the mention in the REG_EQUIV note would be to an uninitialized
	 pseudo.  */

      if (MEM_P (dest) && REG_P (src)
	  && (regno = REGNO (src)) >= FIRST_PSEUDO_REGISTER
	  && REG_BASIC_BLOCK (regno) >= NUM_FIXED_BLOCKS
	  && DF_REG_DEF_COUNT (regno) == 1
	  && reg_equiv[regno].init_insns != 0
	  && reg_equiv[regno].init_insns != const0_rtx
	  && ! find_reg_note (XEXP (reg_equiv[regno].init_insns, 0),
			      REG_EQUIV, NULL_RTX)
	  && ! contains_replace_regs (XEXP (dest, 0)))
	{
	  rtx init_insn = XEXP (reg_equiv[regno].init_insns, 0);
	  if (validate_equiv_mem (init_insn, src, dest)
	      && ! memref_used_between_p (dest, init_insn, insn)
	      /* Attaching a REG_EQUIV note will fail if INIT_INSN has
		 multiple sets.  */
	      && set_unique_reg_note (init_insn, REG_EQUIV, copy_rtx (dest)))
	    {
	      /* This insn makes the equivalence, not the one initializing
		 the register.  */
	      reg_equiv_init (regno)
		= gen_rtx_INSN_LIST (VOIDmode, insn, NULL_RTX);
	      df_notes_rescan (init_insn);
	    }
	}
    }

  cleared_regs = BITMAP_ALLOC (NULL);
  /* Now scan all regs killed in an insn to see if any of them are
     registers only used that once.  If so, see if we can replace the
     reference with the equivalent form.  If we can, delete the
     initializing reference and this register will go away.  If we
     can't replace the reference, and the initializing reference is
     within the same loop (or in an inner loop), then move the register
     initialization just before the use, so that they are in the same
     basic block.  */
  FOR_EACH_BB_REVERSE (bb)
    {
      loop_depth = bb->loop_depth;
      for (insn = BB_END (bb);
	   insn != PREV_INSN (BB_HEAD (bb));
	   insn = PREV_INSN (insn))
	{
	  rtx link;

	  if (! INSN_P (insn))
	    continue;

	  /* Don't substitute into a non-local goto, this confuses CFG.  */
	  if (JUMP_P (insn)
	      && find_reg_note (insn, REG_NON_LOCAL_GOTO, NULL_RTX))
	    continue;

	  for (link = REG_NOTES (insn); link; link = XEXP (link, 1))
	    {
	      if (REG_NOTE_KIND (link) == REG_DEAD
		  /* Make sure this insn still refers to the register.  */
		  && reg_mentioned_p (XEXP (link, 0), PATTERN (insn)))
		{
		  int regno = REGNO (XEXP (link, 0));
		  rtx equiv_insn;

		  if (! reg_equiv[regno].replace
		      || reg_equiv[regno].loop_depth < loop_depth
		      /* There is no sense to move insns if we did
			 register pressure-sensitive scheduling was
			 done because it will not improve allocation
			 but worsen insn schedule with a big
			 probability.  */
		      || (flag_sched_pressure && flag_schedule_insns))
		    continue;

		  /* reg_equiv[REGNO].replace gets set only when
		     REG_N_REFS[REGNO] is 2, i.e. the register is set
		     once and used once.  (If it were only set, but not used,
		     flow would have deleted the setting insns.)  Hence
		     there can only be one insn in reg_equiv[REGNO].init_insns.  */
		  gcc_assert (reg_equiv[regno].init_insns
			      && !XEXP (reg_equiv[regno].init_insns, 1));
		  equiv_insn = XEXP (reg_equiv[regno].init_insns, 0);

		  /* We may not move instructions that can throw, since
		     that changes basic block boundaries and we are not
		     prepared to adjust the CFG to match.  */
		  if (can_throw_internal (equiv_insn))
		    continue;

		  if (asm_noperands (PATTERN (equiv_insn)) < 0
		      && validate_replace_rtx (regno_reg_rtx[regno],
					       *(reg_equiv[regno].src_p), insn))
		    {
		      rtx equiv_link;
		      rtx last_link;
		      rtx note;

		      /* Find the last note.  */
		      for (last_link = link; XEXP (last_link, 1);
			   last_link = XEXP (last_link, 1))
			;

		      /* Append the REG_DEAD notes from equiv_insn.  */
		      equiv_link = REG_NOTES (equiv_insn);
		      while (equiv_link)
			{
			  note = equiv_link;
			  equiv_link = XEXP (equiv_link, 1);
			  if (REG_NOTE_KIND (note) == REG_DEAD)
			    {
			      remove_note (equiv_insn, note);
			      XEXP (last_link, 1) = note;
			      XEXP (note, 1) = NULL_RTX;
			      last_link = note;
			    }
			}

		      remove_death (regno, insn);
		      SET_REG_N_REFS (regno, 0);
		      REG_FREQ (regno) = 0;
		      delete_insn (equiv_insn);

		      reg_equiv[regno].init_insns
			= XEXP (reg_equiv[regno].init_insns, 1);

		      reg_equiv_init (regno) = NULL_RTX;
		      bitmap_set_bit (cleared_regs, regno);
		    }
		  /* Move the initialization of the register to just before
		     INSN.  Update the flow information.  */
		  else if (prev_nondebug_insn (insn) != equiv_insn)
		    {
		      rtx new_insn;

		      new_insn = emit_insn_before (PATTERN (equiv_insn), insn);
		      REG_NOTES (new_insn) = REG_NOTES (equiv_insn);
		      REG_NOTES (equiv_insn) = 0;
		      /* Rescan it to process the notes.  */
		      df_insn_rescan (new_insn);

		      /* Make sure this insn is recognized before
			 reload begins, otherwise
			 eliminate_regs_in_insn will die.  */
		      INSN_CODE (new_insn) = INSN_CODE (equiv_insn);

		      delete_insn (equiv_insn);

		      XEXP (reg_equiv[regno].init_insns, 0) = new_insn;

		      REG_BASIC_BLOCK (regno) = bb->index;
		      REG_N_CALLS_CROSSED (regno) = 0;
		      REG_FREQ_CALLS_CROSSED (regno) = 0;
		      REG_N_THROWING_CALLS_CROSSED (regno) = 0;
		      REG_LIVE_LENGTH (regno) = 2;

		      if (insn == BB_HEAD (bb))
			BB_HEAD (bb) = PREV_INSN (insn);

		      reg_equiv_init (regno)
			= gen_rtx_INSN_LIST (VOIDmode, new_insn, NULL_RTX);
		      bitmap_set_bit (cleared_regs, regno);
		    }
		}
	    }
	}
    }

  if (!bitmap_empty_p (cleared_regs))
    {
      FOR_EACH_BB (bb)
	{
	  bitmap_and_compl_into (DF_LIVE_IN (bb), cleared_regs);
	  bitmap_and_compl_into (DF_LIVE_OUT (bb), cleared_regs);
	  bitmap_and_compl_into (DF_LR_IN (bb), cleared_regs);
	  bitmap_and_compl_into (DF_LR_OUT (bb), cleared_regs);
	}

      /* Last pass - adjust debug insns referencing cleared regs.  */
      if (MAY_HAVE_DEBUG_INSNS)
	for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
	  if (DEBUG_INSN_P (insn))
	    {
	      rtx old_loc = INSN_VAR_LOCATION_LOC (insn);
	      INSN_VAR_LOCATION_LOC (insn)
		= simplify_replace_fn_rtx (old_loc, NULL_RTX,
					   adjust_cleared_regs,
					   (void *) cleared_regs);
	      if (old_loc != INSN_VAR_LOCATION_LOC (insn))
		df_insn_rescan (insn);
	    }
    }

  BITMAP_FREE (cleared_regs);

  out:
  /* Clean up.  */

  end_alias_analysis ();
  free (reg_equiv);
  return recorded_label_ref;
}



/* Print chain C to FILE.  */
static void
print_insn_chain (FILE *file, struct insn_chain *c)
{
  fprintf (file, "insn=%d, ", INSN_UID(c->insn));
  bitmap_print (file, &c->live_throughout, "live_throughout: ", ", ");
  bitmap_print (file, &c->dead_or_set, "dead_or_set: ", "\n");
}


/* Print all reload_insn_chains to FILE.  */
static void
print_insn_chains (FILE *file)
{
  struct insn_chain *c;
  for (c = reload_insn_chain; c ; c = c->next)
    print_insn_chain (file, c);
}

/* Return true if pseudo REGNO should be added to set live_throughout
   or dead_or_set of the insn chains for reload consideration.  */
static bool
pseudo_for_reload_consideration_p (int regno)
{
  /* Consider spilled pseudos too for IRA because they still have a
     chance to get hard-registers in the reload when IRA is used.  */
  return (reg_renumber[regno] >= 0 || ira_conflicts_p);
}

/* Init LIVE_SUBREGS[ALLOCNUM] and LIVE_SUBREGS_USED[ALLOCNUM] using
   REG to the number of nregs, and INIT_VALUE to get the
   initialization.  ALLOCNUM need not be the regno of REG.  */
static void
init_live_subregs (bool init_value, sbitmap *live_subregs,
		   int *live_subregs_used, int allocnum, rtx reg)
{
  unsigned int regno = REGNO (SUBREG_REG (reg));
  int size = GET_MODE_SIZE (GET_MODE (regno_reg_rtx[regno]));

  gcc_assert (size > 0);

  /* Been there, done that.  */
  if (live_subregs_used[allocnum])
    return;

  /* Create a new one with zeros.  */
  if (live_subregs[allocnum] == NULL)
    live_subregs[allocnum] = sbitmap_alloc (size);

  /* If the entire reg was live before blasting into subregs, we need
     to init all of the subregs to ones else init to 0.  */
  if (init_value)
    sbitmap_ones (live_subregs[allocnum]);
  else
    sbitmap_zero (live_subregs[allocnum]);

  /* Set the number of bits that we really want.  */
  live_subregs_used[allocnum] = size;
}

/* Walk the insns of the current function and build reload_insn_chain,
   and record register life information.  */
static void
build_insn_chain (void)
{
  unsigned int i;
  struct insn_chain **p = &reload_insn_chain;
  basic_block bb;
  struct insn_chain *c = NULL;
  struct insn_chain *next = NULL;
  bitmap live_relevant_regs = BITMAP_ALLOC (NULL);
  bitmap elim_regset = BITMAP_ALLOC (NULL);
  /* live_subregs is a vector used to keep accurate information about
     which hardregs are live in multiword pseudos.  live_subregs and
     live_subregs_used are indexed by pseudo number.  The live_subreg
     entry for a particular pseudo is only used if the corresponding
     element is non zero in live_subregs_used.  The value in
     live_subregs_used is number of bytes that the pseudo can
     occupy.  */
  sbitmap *live_subregs = XCNEWVEC (sbitmap, max_regno);
  int *live_subregs_used = XNEWVEC (int, max_regno);

  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
    if (TEST_HARD_REG_BIT (eliminable_regset, i))
      bitmap_set_bit (elim_regset, i);
  FOR_EACH_BB_REVERSE (bb)
    {
      bitmap_iterator bi;
      rtx insn;

      CLEAR_REG_SET (live_relevant_regs);
      memset (live_subregs_used, 0, max_regno * sizeof (int));

      EXECUTE_IF_SET_IN_BITMAP (DF_LR_OUT (bb), 0, i, bi)
	{
	  if (i >= FIRST_PSEUDO_REGISTER)
	    break;
	  bitmap_set_bit (live_relevant_regs, i);
	}

      EXECUTE_IF_SET_IN_BITMAP (DF_LR_OUT (bb),
				FIRST_PSEUDO_REGISTER, i, bi)
	{
	  if (pseudo_for_reload_consideration_p (i))
	    bitmap_set_bit (live_relevant_regs, i);
	}

      FOR_BB_INSNS_REVERSE (bb, insn)
	{
	  if (!NOTE_P (insn) && !BARRIER_P (insn))
	    {
	      unsigned int uid = INSN_UID (insn);
	      df_ref *def_rec;
	      df_ref *use_rec;

	      c = new_insn_chain ();
	      c->next = next;
	      next = c;
	      *p = c;
	      p = &c->prev;

	      c->insn = insn;
	      c->block = bb->index;

	      if (INSN_P (insn))
		for (def_rec = DF_INSN_UID_DEFS (uid); *def_rec; def_rec++)
		  {
		    df_ref def = *def_rec;
		    unsigned int regno = DF_REF_REGNO (def);

		    /* Ignore may clobbers because these are generated
		       from calls. However, every other kind of def is
		       added to dead_or_set.  */
		    if (!DF_REF_FLAGS_IS_SET (def, DF_REF_MAY_CLOBBER))
		      {
			if (regno < FIRST_PSEUDO_REGISTER)
			  {
			    if (!fixed_regs[regno])
			      bitmap_set_bit (&c->dead_or_set, regno);
			  }
			else if (pseudo_for_reload_consideration_p (regno))
			  bitmap_set_bit (&c->dead_or_set, regno);
		      }

		    if ((regno < FIRST_PSEUDO_REGISTER
			 || reg_renumber[regno] >= 0
			 || ira_conflicts_p)
			&& (!DF_REF_FLAGS_IS_SET (def, DF_REF_CONDITIONAL)))
		      {
			rtx reg = DF_REF_REG (def);

			/* We can model subregs, but not if they are
			   wrapped in ZERO_EXTRACTS.  */
			if (GET_CODE (reg) == SUBREG
			    && !DF_REF_FLAGS_IS_SET (def, DF_REF_ZERO_EXTRACT))
			  {
			    unsigned int start = SUBREG_BYTE (reg);
			    unsigned int last = start
			      + GET_MODE_SIZE (GET_MODE (reg));

			    init_live_subregs
			      (bitmap_bit_p (live_relevant_regs, regno),
			       live_subregs, live_subregs_used, regno, reg);

			    if (!DF_REF_FLAGS_IS_SET
				(def, DF_REF_STRICT_LOW_PART))
			      {
				/* Expand the range to cover entire words.
				   Bytes added here are "don't care".  */
				start
				  = start / UNITS_PER_WORD * UNITS_PER_WORD;
				last = ((last + UNITS_PER_WORD - 1)
					/ UNITS_PER_WORD * UNITS_PER_WORD);
			      }

			    /* Ignore the paradoxical bits.  */
			    if ((int)last > live_subregs_used[regno])
			      last = live_subregs_used[regno];

			    while (start < last)
			      {
				RESET_BIT (live_subregs[regno], start);
				start++;
			      }

			    if (sbitmap_empty_p (live_subregs[regno]))
			      {
				live_subregs_used[regno] = 0;
				bitmap_clear_bit (live_relevant_regs, regno);
			      }
			    else
			      /* Set live_relevant_regs here because
				 that bit has to be true to get us to
				 look at the live_subregs fields.  */
			      bitmap_set_bit (live_relevant_regs, regno);
			  }
			else
			  {
			    /* DF_REF_PARTIAL is generated for
			       subregs, STRICT_LOW_PART, and
			       ZERO_EXTRACT.  We handle the subreg
			       case above so here we have to keep from
			       modeling the def as a killing def.  */
			    if (!DF_REF_FLAGS_IS_SET (def, DF_REF_PARTIAL))
			      {
				bitmap_clear_bit (live_relevant_regs, regno);
				live_subregs_used[regno] = 0;
			      }
			  }
		      }
		  }

	      bitmap_and_compl_into (live_relevant_regs, elim_regset);
	      bitmap_copy (&c->live_throughout, live_relevant_regs);

	      if (INSN_P (insn))
		for (use_rec = DF_INSN_UID_USES (uid); *use_rec; use_rec++)
		  {
		    df_ref use = *use_rec;
		    unsigned int regno = DF_REF_REGNO (use);
		    rtx reg = DF_REF_REG (use);

		    /* DF_REF_READ_WRITE on a use means that this use
		       is fabricated from a def that is a partial set
		       to a multiword reg.  Here, we only model the
		       subreg case that is not wrapped in ZERO_EXTRACT
		       precisely so we do not need to look at the
		       fabricated use. */
		    if (DF_REF_FLAGS_IS_SET (use, DF_REF_READ_WRITE)
			&& !DF_REF_FLAGS_IS_SET (use, DF_REF_ZERO_EXTRACT)
			&& DF_REF_FLAGS_IS_SET (use, DF_REF_SUBREG))
		      continue;

		    /* Add the last use of each var to dead_or_set.  */
		    if (!bitmap_bit_p (live_relevant_regs, regno))
		      {
			if (regno < FIRST_PSEUDO_REGISTER)
			  {
			    if (!fixed_regs[regno])
			      bitmap_set_bit (&c->dead_or_set, regno);
			  }
			else if (pseudo_for_reload_consideration_p (regno))
			  bitmap_set_bit (&c->dead_or_set, regno);
		      }

		    if (regno < FIRST_PSEUDO_REGISTER
			|| pseudo_for_reload_consideration_p (regno))
		      {
			if (GET_CODE (reg) == SUBREG
			    && !DF_REF_FLAGS_IS_SET (use,
						     DF_REF_SIGN_EXTRACT
						     | DF_REF_ZERO_EXTRACT))
			  {
			    unsigned int start = SUBREG_BYTE (reg);
			    unsigned int last = start
			      + GET_MODE_SIZE (GET_MODE (reg));

			    init_live_subregs
			      (bitmap_bit_p (live_relevant_regs, regno),
			       live_subregs, live_subregs_used, regno, reg);

			    /* Ignore the paradoxical bits.  */
			    if ((int)last > live_subregs_used[regno])
			      last = live_subregs_used[regno];

			    while (start < last)
			      {
				SET_BIT (live_subregs[regno], start);
				start++;
			      }
			  }
			else
			  /* Resetting the live_subregs_used is
			     effectively saying do not use the subregs
			     because we are reading the whole
			     pseudo.  */
			  live_subregs_used[regno] = 0;
			bitmap_set_bit (live_relevant_regs, regno);
		      }
		  }
	    }
	}

      /* FIXME!! The following code is a disaster.  Reload needs to see the
	 labels and jump tables that are just hanging out in between
	 the basic blocks.  See pr33676.  */
      insn = BB_HEAD (bb);

      /* Skip over the barriers and cruft.  */
      while (insn && (BARRIER_P (insn) || NOTE_P (insn)
		      || BLOCK_FOR_INSN (insn) == bb))
	insn = PREV_INSN (insn);

      /* While we add anything except barriers and notes, the focus is
	 to get the labels and jump tables into the
	 reload_insn_chain.  */
      while (insn)
	{
	  if (!NOTE_P (insn) && !BARRIER_P (insn))
	    {
	      if (BLOCK_FOR_INSN (insn))
		break;

	      c = new_insn_chain ();
	      c->next = next;
	      next = c;
	      *p = c;
	      p = &c->prev;

	      /* The block makes no sense here, but it is what the old
		 code did.  */
	      c->block = bb->index;
	      c->insn = insn;
	      bitmap_copy (&c->live_throughout, live_relevant_regs);
	    }
	  insn = PREV_INSN (insn);
	}
    }

  for (i = 0; i < (unsigned int) max_regno; i++)
    free (live_subregs[i]);

  reload_insn_chain = c;
  *p = NULL;

  free (live_subregs);
  free (live_subregs_used);
  BITMAP_FREE (live_relevant_regs);
  BITMAP_FREE (elim_regset);

  if (dump_file)
    print_insn_chains (dump_file);
}



/* All natural loops.  */
struct loops ira_loops;

/* True if we have allocno conflicts.  It is false for non-optimized
   mode or when the conflict table is too big.  */
bool ira_conflicts_p;

/* Saved between IRA and reload.  */
static int saved_flag_ira_share_spill_slots;

/* This is the main entry of IRA.  */
static void
ira (FILE *f)
{
  int allocated_reg_info_size;
  bool loops_p;
  int max_regno_before_ira, ira_max_point_before_emit;
  int rebuild_p;

  if (flag_caller_saves)
    init_caller_save ();

  if (flag_ira_verbose < 10)
    {
      internal_flag_ira_verbose = flag_ira_verbose;
      ira_dump_file = f;
    }
  else
    {
      internal_flag_ira_verbose = flag_ira_verbose - 10;
      ira_dump_file = stderr;
    }

  ira_conflicts_p = optimize > 0;
  setup_prohibited_mode_move_regs ();

  df_note_add_problem ();

  if (optimize == 1)
    {
      df_live_add_problem ();
      df_live_set_all_dirty ();
    }
#ifdef ENABLE_CHECKING
  df->changeable_flags |= DF_VERIFY_SCHEDULED;
#endif
  df_analyze ();
  df_clear_flags (DF_NO_INSN_RESCAN);
  regstat_init_n_sets_and_refs ();
  regstat_compute_ri ();

  /* If we are not optimizing, then this is the only place before
     register allocation where dataflow is done.  And that is needed
     to generate these warnings.  */
  if (warn_clobbered)
    generate_setjmp_warnings ();

  /* Determine if the current function is a leaf before running IRA
     since this can impact optimizations done by the prologue and
     epilogue thus changing register elimination offsets.  */
  current_function_is_leaf = leaf_function_p ();

  if (resize_reg_info () && flag_ira_loop_pressure)
    ira_set_pseudo_classes (ira_dump_file);

  rebuild_p = update_equiv_regs ();

#ifndef IRA_NO_OBSTACK
  gcc_obstack_init (&ira_obstack);
#endif
  bitmap_obstack_initialize (&ira_bitmap_obstack);
  if (optimize)
    {
      max_regno = max_reg_num ();
      ira_reg_equiv_len = max_regno;
      ira_reg_equiv_invariant_p
	= (bool *) ira_allocate (max_regno * sizeof (bool));
      memset (ira_reg_equiv_invariant_p, 0, max_regno * sizeof (bool));
      ira_reg_equiv_const = (rtx *) ira_allocate (max_regno * sizeof (rtx));
      memset (ira_reg_equiv_const, 0, max_regno * sizeof (rtx));
      find_reg_equiv_invariant_const ();
      if (rebuild_p)
	{
	  timevar_push (TV_JUMP);
	  rebuild_jump_labels (get_insns ());
	  if (purge_all_dead_edges ())
	    delete_unreachable_blocks ();
	  timevar_pop (TV_JUMP);
	}
    }

  max_regno_before_ira = allocated_reg_info_size = max_reg_num ();
  ira_setup_eliminable_regset ();

  ira_overall_cost = ira_reg_cost = ira_mem_cost = 0;
  ira_load_cost = ira_store_cost = ira_shuffle_cost = 0;
  ira_move_loops_num = ira_additional_jumps_num = 0;

  ira_assert (current_loops == NULL);
  if (flag_ira_region == IRA_REGION_ALL || flag_ira_region == IRA_REGION_MIXED)
    {
      flow_loops_find (&ira_loops);
      record_loop_exits ();
      current_loops = &ira_loops;
    }

  if (internal_flag_ira_verbose > 0 && ira_dump_file != NULL)
    fprintf (ira_dump_file, "Building IRA IR\n");
  loops_p = ira_build ();

  ira_assert (ira_conflicts_p || !loops_p);

  saved_flag_ira_share_spill_slots = flag_ira_share_spill_slots;
  if (too_high_register_pressure_p () || cfun->calls_setjmp)
    /* It is just wasting compiler's time to pack spilled pseudos into
       stack slots in this case -- prohibit it.  We also do this if
       there is setjmp call because a variable not modified between
       setjmp and longjmp the compiler is required to preserve its
       value and sharing slots does not guarantee it.  */
    flag_ira_share_spill_slots = FALSE;

  ira_color ();

  ira_max_point_before_emit = ira_max_point;

  ira_initiate_emit_data ();

  ira_emit (loops_p);

  if (ira_conflicts_p)
    {
      max_regno = max_reg_num ();

      if (! loops_p)
	ira_initiate_assign ();
      else
	{
	  expand_reg_info (allocated_reg_info_size);
	  setup_preferred_alternate_classes_for_new_pseudos
	    (allocated_reg_info_size);
	  allocated_reg_info_size = max_regno;

	  if (internal_flag_ira_verbose > 0 && ira_dump_file != NULL)
	    fprintf (ira_dump_file, "Flattening IR\n");
	  ira_flattening (max_regno_before_ira, ira_max_point_before_emit);
	  /* New insns were generated: add notes and recalculate live
	     info.  */
	  df_analyze ();

	  flow_loops_find (&ira_loops);
	  record_loop_exits ();
	  current_loops = &ira_loops;

	  setup_allocno_assignment_flags ();
	  ira_initiate_assign ();
	  ira_reassign_conflict_allocnos (max_regno);
	}
    }

  ira_finish_emit_data ();

  setup_reg_renumber ();

  calculate_allocation_cost ();

#ifdef ENABLE_IRA_CHECKING
  if (ira_conflicts_p)
    check_allocation ();
#endif

  if (delete_trivially_dead_insns (get_insns (), max_reg_num ()))
    df_analyze ();

  if (max_regno != max_regno_before_ira)
    {
      regstat_free_n_sets_and_refs ();
      regstat_free_ri ();
      regstat_init_n_sets_and_refs ();
      regstat_compute_ri ();
    }

  overall_cost_before = ira_overall_cost;
  if (! ira_conflicts_p)
    grow_reg_equivs ();
  else
    {
      fix_reg_equiv_init ();

#ifdef ENABLE_IRA_CHECKING
      print_redundant_copies ();
#endif

      ira_spilled_reg_stack_slots_num = 0;
      ira_spilled_reg_stack_slots
	= ((struct ira_spilled_reg_stack_slot *)
	   ira_allocate (max_regno
			 * sizeof (struct ira_spilled_reg_stack_slot)));
      memset (ira_spilled_reg_stack_slots, 0,
	      max_regno * sizeof (struct ira_spilled_reg_stack_slot));
    }
  allocate_initial_values (reg_equivs);
}

static void
do_reload (void)
{
  basic_block bb;
  bool need_dce;

  if (flag_ira_verbose < 10)
    ira_dump_file = dump_file;

  df_set_flags (DF_NO_INSN_RESCAN);
  build_insn_chain ();

  need_dce = reload (get_insns (), ira_conflicts_p);

  timevar_push (TV_IRA);

  if (ira_conflicts_p)
    {
      ira_free (ira_spilled_reg_stack_slots);

      ira_finish_assign ();
    }
  if (internal_flag_ira_verbose > 0 && ira_dump_file != NULL
      && overall_cost_before != ira_overall_cost)
    fprintf (ira_dump_file, "+++Overall after reload %d\n", ira_overall_cost);
  ira_destroy ();

  flag_ira_share_spill_slots = saved_flag_ira_share_spill_slots;

  if (current_loops != NULL)
    {
      flow_loops_free (&ira_loops);
      free_dominance_info (CDI_DOMINATORS);
    }
  FOR_ALL_BB (bb)
    bb->loop_father = NULL;
  current_loops = NULL;

  regstat_free_ri ();
  regstat_free_n_sets_and_refs ();

  if (optimize)
    {
      cleanup_cfg (CLEANUP_EXPENSIVE);

      ira_free (ira_reg_equiv_invariant_p);
      ira_free (ira_reg_equiv_const);
    }

  bitmap_obstack_release (&ira_bitmap_obstack);
#ifndef IRA_NO_OBSTACK
  obstack_free (&ira_obstack, NULL);
#endif

  /* The code after the reload has changed so much that at this point
     we might as well just rescan everything.  Note that
     df_rescan_all_insns is not going to help here because it does not
     touch the artificial uses and defs.  */
  df_finish_pass (true);
  if (optimize > 1)
    df_live_add_problem ();
  df_scan_alloc (NULL);
  df_scan_blocks ();

  if (optimize)
    df_analyze ();

  if (need_dce && optimize)
    run_fast_dce ();

  timevar_pop (TV_IRA);
}

/* Run the integrated register allocator.  */
static unsigned int
rest_of_handle_ira (void)
{
  ira (dump_file);
  return 0;
}

struct rtl_opt_pass pass_ira =
{
 {
  RTL_PASS,
  "ira",                                /* name */
  NULL,                                 /* gate */
  rest_of_handle_ira,		        /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  TV_IRA,	                        /* tv_id */
  0,                                    /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_dump_func                        /* todo_flags_finish */
 }
};

static unsigned int
rest_of_handle_reload (void)
{
  do_reload ();
  return 0;
}

struct rtl_opt_pass pass_reload =
{
 {
  RTL_PASS,
  "reload",                             /* name */
  NULL,                                 /* gate */
  rest_of_handle_reload,	        /* execute */
  NULL,                                 /* sub */
  NULL,                                 /* next */
  0,                                    /* static_pass_number */
  TV_RELOAD,	                        /* tv_id */
  0,                                    /* properties_required */
  0,                                    /* properties_provided */
  0,                                    /* properties_destroyed */
  0,                                    /* todo_flags_start */
  TODO_dump_func | TODO_ggc_collect     /* todo_flags_finish */
 }
};
