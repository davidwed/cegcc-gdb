/* OpenCL language support for GDB, the GNU debugger.
   Copyright (C) 2010 Free Software Foundation, Inc.

   Contributed by Ken Werner <ken.werner@de.ibm.com>.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "gdb_string.h"
#include "gdbtypes.h"
#include "symtab.h"
#include "expression.h"
#include "parser-defs.h"
#include "symtab.h"
#include "language.h"
#include "c-lang.h"
#include "gdb_assert.h"

extern void _initialize_opencl_language (void);

/* This macro generates enum values from a given type.  */

#define OCL_P_TYPE(TYPE)\
  opencl_primitive_type_##TYPE,\
  opencl_primitive_type_##TYPE##2,\
  opencl_primitive_type_##TYPE##3,\
  opencl_primitive_type_##TYPE##4,\
  opencl_primitive_type_##TYPE##8,\
  opencl_primitive_type_##TYPE##16

enum opencl_primitive_types {
  OCL_P_TYPE (char),
  OCL_P_TYPE (uchar),
  OCL_P_TYPE (short),
  OCL_P_TYPE (ushort),
  OCL_P_TYPE (int),
  OCL_P_TYPE (uint),
  OCL_P_TYPE (long),
  OCL_P_TYPE (ulong),
  OCL_P_TYPE (half),
  OCL_P_TYPE (float),
  OCL_P_TYPE (double),
  opencl_primitive_type_bool,
  opencl_primitive_type_unsigned_char,
  opencl_primitive_type_unsigned_short,
  opencl_primitive_type_unsigned_int,
  opencl_primitive_type_unsigned_long,
  opencl_primitive_type_size_t,
  opencl_primitive_type_ptrdiff_t,
  opencl_primitive_type_intptr_t,
  opencl_primitive_type_uintptr_t,
  opencl_primitive_type_void,
  nr_opencl_primitive_types
};

/* This macro generates the type struct declarations from a given type.  */

#define STRUCT_OCL_TYPE(TYPE)\
  struct type *builtin_##TYPE;\
  struct type *builtin_##TYPE##2;\
  struct type *builtin_##TYPE##3;\
  struct type *builtin_##TYPE##4;\
  struct type *builtin_##TYPE##8;\
  struct type *builtin_##TYPE##16

struct builtin_opencl_type
{
  STRUCT_OCL_TYPE (char);
  STRUCT_OCL_TYPE (uchar);
  STRUCT_OCL_TYPE (short);
  STRUCT_OCL_TYPE (ushort);
  STRUCT_OCL_TYPE (int);
  STRUCT_OCL_TYPE (uint);
  STRUCT_OCL_TYPE (long);
  STRUCT_OCL_TYPE (ulong);
  STRUCT_OCL_TYPE (half);
  STRUCT_OCL_TYPE (float);
  STRUCT_OCL_TYPE (double);
  struct type *builtin_bool;
  struct type *builtin_unsigned_char;
  struct type *builtin_unsigned_short;
  struct type *builtin_unsigned_int;
  struct type *builtin_unsigned_long;
  struct type *builtin_size_t;
  struct type *builtin_ptrdiff_t;
  struct type *builtin_intptr_t;
  struct type *builtin_uintptr_t;
  struct type *builtin_void;
};

static struct gdbarch_data *opencl_type_data;

const struct builtin_opencl_type *
builtin_opencl_type (struct gdbarch *gdbarch)
{
  return gdbarch_data (gdbarch, opencl_type_data);
}

/* Returns the corresponding OpenCL vector type from the given type code,
   the length of the element type, the unsigned flag and the amount of
   elements (N).  */

static struct type *
lookup_opencl_vector_type (struct gdbarch *gdbarch, enum type_code code,
			   unsigned int el_length, unsigned int flag_unsigned,
			   int n)
{
  int i;
  unsigned int length;
  struct type *type = NULL;
  struct type **types = (struct type **) builtin_opencl_type (gdbarch);

  /* Check if n describes a valid OpenCL vector size (2, 3, 4, 8, 16).  */
  if (n != 2 && n != 3 && n != 4 && n != 8 && n != 16)
    error (_("Invalid OpenCL vector size: %d"), n);

  /* Triple vectors have the size of a quad vector.  */
  length = (n == 3) ?  el_length * 4 : el_length * n;

  for (i = 0; i < nr_opencl_primitive_types; i++)
    {
      LONGEST lowb, highb;

      if (TYPE_CODE (types[i]) == TYPE_CODE_ARRAY && TYPE_VECTOR (types[i])
	  && get_array_bounds (types[i], &lowb, &highb)
	  && TYPE_CODE (TYPE_TARGET_TYPE (types[i])) == code
	  && TYPE_UNSIGNED (TYPE_TARGET_TYPE (types[i])) == flag_unsigned
	  && TYPE_LENGTH (TYPE_TARGET_TYPE (types[i])) == el_length
	  && TYPE_LENGTH (types[i]) == length
	  && highb - lowb + 1 == n)
	{
	  type = types[i];
	  break;
	}
    }

  return type;
}

/* Returns nonzero if the array ARR contains duplicates within
     the first N elements.  */

static int
array_has_dups (int *arr, int n)
{
  int i, j;

  for (i = 0; i < n; i++)
    {
      for (j = i + 1; j < n; j++)
        {
          if (arr[i] == arr[j])
            return 1;
        }
    }

  return 0;
}

/* The OpenCL component access syntax allows to create lvalues referring to
   selected elements of an original OpenCL vector in arbitrary order.  This
   structure holds the information to describe such lvalues.  */

struct lval_closure
{
  /* Reference count.  */
  int refc;
  /* The number of indices.  */
  int n;
  /* The element indices themselves.  */
  int *indices;
  /* A pointer to the original value.  */
  struct value *val;
};

/* Allocates an instance of struct lval_closure.  */

static struct lval_closure *
allocate_lval_closure (int *indices, int n, struct value *val)
{
  struct lval_closure *c = XZALLOC (struct lval_closure);

  c->refc = 1;
  c->n = n;
  c->indices = XCALLOC (n, int);
  memcpy (c->indices, indices, n * sizeof (int));
  value_incref (val); /* Increment the reference counter of the value.  */
  c->val = val;

  return c;
}

static void
lval_func_read (struct value *v)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);
  struct type *type = check_typedef (value_type (v));
  struct type *eltype = TYPE_TARGET_TYPE (check_typedef (value_type (c->val)));
  int offset = value_offset (v);
  int elsize = TYPE_LENGTH (eltype);
  int n, i, j = 0;
  LONGEST lowb = 0;
  LONGEST highb = 0;

  if (TYPE_CODE (type) == TYPE_CODE_ARRAY
      && !get_array_bounds (type, &lowb, &highb))
    error (_("Could not determine the vector bounds"));

  /* Assume elsize aligned offset.  */
  gdb_assert (offset % elsize == 0);
  offset /= elsize;
  n = offset + highb - lowb + 1;
  gdb_assert (n <= c->n);

  for (i = offset; i < n; i++)
    memcpy (value_contents_raw (v) + j++ * elsize,
	    value_contents (c->val) + c->indices[i] * elsize,
	    elsize);
}

static void
lval_func_write (struct value *v, struct value *fromval)
{
  struct value *mark = value_mark ();
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);
  struct type *type = check_typedef (value_type (v));
  struct type *eltype = TYPE_TARGET_TYPE (check_typedef (value_type (c->val)));
  int offset = value_offset (v);
  int elsize = TYPE_LENGTH (eltype);
  int n, i, j = 0;
  LONGEST lowb = 0;
  LONGEST highb = 0;

  if (TYPE_CODE (type) == TYPE_CODE_ARRAY
      && !get_array_bounds (type, &lowb, &highb))
    error (_("Could not determine the vector bounds"));

  /* Assume elsize aligned offset.  */
  gdb_assert (offset % elsize == 0);
  offset /= elsize;
  n = offset + highb - lowb + 1;

  /* Since accesses to the fourth component of a triple vector is undefined we
     just skip writes to the fourth element.  Imagine something like this:
       int3 i3 = (int3)(0, 1, 2);
       i3.hi.hi = 5;
     In this case n would be 4 (offset=12/4 + 1) while c->n would be 3.  */
  if (n > c->n)
    n = c->n;

  for (i = offset; i < n; i++)
    {
      struct value *from_elm_val = allocate_value (eltype);
      struct value *to_elm_val = value_subscript (c->val, c->indices[i]);

      memcpy (value_contents_writeable (from_elm_val),
	      value_contents (fromval) + j++ * elsize,
	      elsize);
      value_assign (to_elm_val, from_elm_val);
    }

  value_free_to_mark (mark);
}

/* Return nonzero if all bits in V within OFFSET and LENGTH are valid.  */

static int
lval_func_check_validity (const struct value *v, int offset, int length)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);
  /* Size of the target type in bits.  */
  int elsize =
      TYPE_LENGTH (TYPE_TARGET_TYPE (check_typedef (value_type (c->val)))) * 8;
  int startrest = offset % elsize;
  int start = offset / elsize;
  int endrest = (offset + length) % elsize;
  int end = (offset + length) / elsize;
  int i;

  if (endrest)
    end++;

  if (end > c->n)
    return 0;

  for (i = start; i < end; i++)
    {
      int startoffset = (i == start) ? startrest : 0;
      int length = (i == end) ? endrest : elsize;

      if (!value_bits_valid (c->val, c->indices[i] * elsize + startoffset,
			     length))
	return 0;
    }

  return 1;
}

/* Return nonzero if any bit in V is valid.  */

static int
lval_func_check_any_valid (const struct value *v)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);
  /* Size of the target type in bits.  */
  int elsize =
      TYPE_LENGTH (TYPE_TARGET_TYPE (check_typedef (value_type (c->val)))) * 8;
  int i;

  for (i = 0; i < c->n; i++)
    if (value_bits_valid (c->val, c->indices[i] * elsize, elsize))
      return 1;

  return 0;
}

/* Return nonzero if bits in V from OFFSET and LENGTH represent a
   synthetic pointer.  */

static int
lval_func_check_synthetic_pointer (const struct value *v,
				   int offset, int length)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);
  /* Size of the target type in bits.  */
  int elsize =
      TYPE_LENGTH (TYPE_TARGET_TYPE (check_typedef (value_type (c->val)))) * 8;
  int startrest = offset % elsize;
  int start = offset / elsize;
  int endrest = (offset + length) % elsize;
  int end = (offset + length) / elsize;
  int i;

  if (endrest)
    end++;

  if (end > c->n)
    return 0;

  for (i = start; i < end; i++)
    {
      int startoffset = (i == start) ? startrest : 0;
      int length = (i == end) ? endrest : elsize;

      if (!value_bits_synthetic_pointer (c->val,
					 c->indices[i] * elsize + startoffset,
					 length))
	return 0;
    }

  return 1;
}

static void *
lval_func_copy_closure (const struct value *v)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);

  ++c->refc;

  return c;
}

static void
lval_func_free_closure (struct value *v)
{
  struct lval_closure *c = (struct lval_closure *) value_computed_closure (v);

  --c->refc;

  if (c->refc == 0)
    {
      xfree (c->indices);
      xfree (c);
      value_free (c->val); /* Decrement the reference counter of the value.  */
    }
}

static struct lval_funcs opencl_value_funcs =
  {
    lval_func_read,
    lval_func_write,
    lval_func_check_validity,
    lval_func_check_any_valid,
    NULL,
    lval_func_check_synthetic_pointer,
    lval_func_copy_closure,
    lval_func_free_closure
  };

/* Creates a sub-vector from VAL.  The elements are selected by the indices of
   an array with the length of N.  Supported values for NOSIDE are
   EVAL_NORMAL and EVAL_AVOID_SIDE_EFFECTS.  */

static struct value *
create_value (struct gdbarch *gdbarch, struct value *val, enum noside noside,
	      int *indices, int n)
{
  struct type *type = check_typedef (value_type (val));
  struct type *elm_type = TYPE_TARGET_TYPE (type);
  struct value *ret;

  /* Check if a single component of a vector is requested which means
     the resulting type is a (primitive) scalar type.  */
  if (n == 1)
    {
      if (noside == EVAL_AVOID_SIDE_EFFECTS)
        ret = value_zero (elm_type, not_lval);
      else
        ret = value_subscript (val, indices[0]);
    }
  else
    {
      /* Multiple components of the vector are requested which means the
	 resulting type is a vector as well.  */
      struct type *dst_type =
	lookup_opencl_vector_type (gdbarch, TYPE_CODE (elm_type),
				   TYPE_LENGTH (elm_type),
				   TYPE_UNSIGNED (elm_type), n);

      if (dst_type == NULL)
	dst_type = init_vector_type (elm_type, n);

      make_cv_type (TYPE_CONST (type), TYPE_VOLATILE (type), dst_type, NULL);

      if (noside == EVAL_AVOID_SIDE_EFFECTS)
	ret = allocate_value (dst_type);
      else
	{
	  /* Check whether to create a lvalue or not.  */
	  if (VALUE_LVAL (val) != not_lval && !array_has_dups (indices, n))
	    {
	      struct lval_closure *c = allocate_lval_closure (indices, n, val);
	      ret = allocate_computed_value (dst_type, &opencl_value_funcs, c);
	    }
	  else
	    {
	      int i;

	      ret = allocate_value (dst_type);

	      /* Copy src val contents into the destination value.  */
	      for (i = 0; i < n; i++)
		memcpy (value_contents_writeable (ret)
			+ (i * TYPE_LENGTH (elm_type)),
			value_contents (val)
			+ (indices[i] * TYPE_LENGTH (elm_type)),
			TYPE_LENGTH (elm_type));
	    }
	}
    }
  return ret;
}

/* OpenCL vector component access.  */

static struct value *
opencl_component_ref (struct expression *exp, struct value *val, char *comps,
		      enum noside noside)
{
  LONGEST lowb, highb;
  int src_len;
  struct value *v;
  int indices[16], i;
  int dst_len;

  if (!get_array_bounds (check_typedef (value_type (val)), &lowb, &highb))
    error (_("Could not determine the vector bounds"));

  src_len = highb - lowb + 1;

  /* Throw an error if the amount of array elements does not fit a
     valid OpenCL vector size (2, 3, 4, 8, 16).  */
  if (src_len != 2 && src_len != 3 && src_len != 4 && src_len != 8
      && src_len != 16)
    error (_("Invalid OpenCL vector size"));

  if (strcmp (comps, "lo") == 0 )
    {
      dst_len = (src_len == 3) ? 2 : src_len / 2;

      for (i = 0; i < dst_len; i++)
	indices[i] = i;
    }
  else if (strcmp (comps, "hi") == 0)
    {
      dst_len = (src_len == 3) ? 2 : src_len / 2;

      for (i = 0; i < dst_len; i++)
	indices[i] = dst_len + i;
    }
  else if (strcmp (comps, "even") == 0)
    {
      dst_len = (src_len == 3) ? 2 : src_len / 2;

      for (i = 0; i < dst_len; i++)
	indices[i] = i*2;
    }
  else if (strcmp (comps, "odd") == 0)
    {
      dst_len = (src_len == 3) ? 2 : src_len / 2;

      for (i = 0; i < dst_len; i++)
        indices[i] = i*2+1;
    }
  else if (strncasecmp (comps, "s", 1) == 0)
    {
#define HEXCHAR_TO_INT(C) ((C >= '0' && C <= '9') ? \
                           C-'0' : ((C >= 'A' && C <= 'F') ? \
                           C-'A'+10 : ((C >= 'a' && C <= 'f') ? \
                           C-'a'+10 : -1)))

      dst_len = strlen (comps);
      /* Skip the s/S-prefix.  */
      dst_len--;

      for (i = 0; i < dst_len; i++)
	{
	  indices[i] = HEXCHAR_TO_INT(comps[i+1]);
	  /* Check if the requested component is invalid or exceeds
	     the vector.  */
	  if (indices[i] < 0 || indices[i] >= src_len)
	    error (_("Invalid OpenCL vector component accessor %s"), comps);
	}
    }
  else
    {
      dst_len = strlen (comps);

      for (i = 0; i < dst_len; i++)
	{
	  /* x, y, z, w */
	  switch (comps[i])
	  {
	  case 'x':
	    indices[i] = 0;
	    break;
	  case 'y':
	    indices[i] = 1;
	    break;
	  case 'z':
	    if (src_len < 3)
	      error (_("Invalid OpenCL vector component accessor %s"), comps);
	    indices[i] = 2;
	    break;
	  case 'w':
	    if (src_len < 4)
	      error (_("Invalid OpenCL vector component accessor %s"), comps);
	    indices[i] = 3;
	    break;
	  default:
	    error (_("Invalid OpenCL vector component accessor %s"), comps);
	    break;
	  }
	}
    }

  /* Throw an error if the amount of requested components does not
     result in a valid length (1, 2, 3, 4, 8, 16).  */
  if (dst_len != 1 && dst_len != 2 && dst_len != 3 && dst_len != 4
      && dst_len != 8 && dst_len != 16)
    error (_("Invalid OpenCL vector component accessor %s"), comps);

  v = create_value (exp->gdbarch, val, noside, indices, dst_len);

  return v;
}

/* Perform the unary logical not (!) operation.  */

static struct value *
opencl_logical_not (struct expression *exp, struct value *arg)
{
  struct type *type = check_typedef (value_type (arg));
  struct type *rettype;
  struct value *ret;

  if (TYPE_CODE (type) == TYPE_CODE_ARRAY && TYPE_VECTOR (type))
    {
      struct type *eltype = check_typedef (TYPE_TARGET_TYPE (type));
      LONGEST lowb, highb;
      int i;

      if (!get_array_bounds (type, &lowb, &highb))
	error (_("Could not determine the vector bounds"));

      /* Determine the resulting type of the operation and allocate the
	 value.  */
      rettype = lookup_opencl_vector_type (exp->gdbarch, TYPE_CODE_INT,
					   TYPE_LENGTH (eltype), 0,
					   highb - lowb + 1);
      ret = allocate_value (rettype);

      for (i = 0; i < highb - lowb + 1; i++)
	{
	  /* For vector types, the unary operator shall return a 0 if the
	  value of its operand compares unequal to 0, and -1 (i.e. all bits
	  set) if the value of its operand compares equal to 0.  */
	  int tmp = value_logical_not (value_subscript (arg, i)) ? -1 : 0;
	  memset (value_contents_writeable (ret) + i * TYPE_LENGTH (eltype),
		  tmp, TYPE_LENGTH (eltype));
	}
    }
  else
    {
      rettype = language_bool_type (exp->language_defn, exp->gdbarch);
      ret = value_from_longest (rettype, value_logical_not (arg));
    }

  return ret;
}

/* Perform a relational operation on two scalar operands.  */

static int
scalar_relop (struct value *val1, struct value *val2, enum exp_opcode op)
{
  int ret;

  switch (op)
    {
    case BINOP_EQUAL:
      ret = value_equal (val1, val2);
      break;
    case BINOP_NOTEQUAL:
      ret = !value_equal (val1, val2);
      break;
    case BINOP_LESS:
      ret = value_less (val1, val2);
      break;
    case BINOP_GTR:
      ret = value_less (val2, val1);
      break;
    case BINOP_GEQ:
      ret = value_less (val2, val1) || value_equal (val1, val2);
      break;
    case BINOP_LEQ:
      ret = value_less (val1, val2) || value_equal (val1, val2);
      break;
    case BINOP_LOGICAL_AND:
      ret = !value_logical_not (val1) && !value_logical_not (val2);
      break;
    case BINOP_LOGICAL_OR:
      ret = !value_logical_not (val1) || !value_logical_not (val2);
      break;
    default:
      error (_("Attempt to perform an unsupported operation"));
      break;
    }
  return ret;
}

/* Perform a relational operation on two vector operands.  */

static struct value *
vector_relop (struct expression *exp, struct value *val1, struct value *val2,
	      enum exp_opcode op)
{
  struct value *ret;
  struct type *type1, *type2, *eltype1, *eltype2, *rettype;
  int t1_is_vec, t2_is_vec, i;
  LONGEST lowb1, lowb2, highb1, highb2;

  type1 = check_typedef (value_type (val1));
  type2 = check_typedef (value_type (val2));

  t1_is_vec = (TYPE_CODE (type1) == TYPE_CODE_ARRAY && TYPE_VECTOR (type1));
  t2_is_vec = (TYPE_CODE (type2) == TYPE_CODE_ARRAY && TYPE_VECTOR (type2));

  if (!t1_is_vec || !t2_is_vec)
    error (_("Vector operations are not supported on scalar types"));

  eltype1 = check_typedef (TYPE_TARGET_TYPE (type1));
  eltype2 = check_typedef (TYPE_TARGET_TYPE (type2));

  if (!get_array_bounds (type1,&lowb1, &highb1)
      || !get_array_bounds (type2, &lowb2, &highb2))
    error (_("Could not determine the vector bounds"));

  /* Check whether the vector types are compatible.  */
  if (TYPE_CODE (eltype1) != TYPE_CODE (eltype2)
      || TYPE_LENGTH (eltype1) != TYPE_LENGTH (eltype2)
      || TYPE_UNSIGNED (eltype1) != TYPE_UNSIGNED (eltype2)
      || lowb1 != lowb2 || highb1 != highb2)
    error (_("Cannot perform operation on vectors with different types"));

  /* Determine the resulting type of the operation and allocate the value.  */
  rettype = lookup_opencl_vector_type (exp->gdbarch, TYPE_CODE_INT,
				       TYPE_LENGTH (eltype1), 0,
				       highb1 - lowb1 + 1);
  ret = allocate_value (rettype);

  for (i = 0; i < highb1 - lowb1 + 1; i++)
    {
      /* For vector types, the relational, equality and logical operators shall
	 return 0 if the specified relation is false and -1 (i.e. all bits set)
	 if the specified relation is true.  */
      int tmp = scalar_relop (value_subscript (val1, i),
			      value_subscript (val2, i), op) ? -1 : 0;
      memset (value_contents_writeable (ret) + i * TYPE_LENGTH (eltype1),
	      tmp, TYPE_LENGTH (eltype1));
     }

  return ret;
}

/* Perform a relational operation on two operands.  */

static struct value *
opencl_relop (struct expression *exp, struct value *arg1, struct value *arg2,
	      enum exp_opcode op)
{
  struct value *val;
  struct type *type1 = check_typedef (value_type (arg1));
  struct type *type2 = check_typedef (value_type (arg2));
  int t1_is_vec = (TYPE_CODE (type1) == TYPE_CODE_ARRAY
		   && TYPE_VECTOR (type1));
  int t2_is_vec = (TYPE_CODE (type2) == TYPE_CODE_ARRAY
		   && TYPE_VECTOR (type2));

  if (!t1_is_vec && !t2_is_vec)
    {
      int tmp = scalar_relop (arg1, arg2, op);
      struct type *type =
	language_bool_type (exp->language_defn, exp->gdbarch);

      val = value_from_longest (type, tmp);
    }
  else if (t1_is_vec && t2_is_vec)
    {
      val = vector_relop (exp, arg1, arg2, op);
    }
  else
    {
      /* Widen the scalar operand to a vector.  */
      struct value **v = t1_is_vec ? &arg2 : &arg1;
      struct type *t = t1_is_vec ? type2 : type1;

      if (TYPE_CODE (t) != TYPE_CODE_FLT && !is_integral_type (t))
	error (_("Argument to operation not a number or boolean."));

      *v = value_cast (t1_is_vec ? type1 : type2, *v);
      val = vector_relop (exp, arg1, arg2, op);
    }

  return val;
}

/* Expression evaluator for the OpenCL.  Most operations are delegated to
   evaluate_subexp_standard; see that function for a description of the
   arguments.  */

static struct value *
evaluate_subexp_opencl (struct type *expect_type, struct expression *exp,
		   int *pos, enum noside noside)
{
  enum exp_opcode op = exp->elts[*pos].opcode;
  struct value *arg1 = NULL;
  struct value *arg2 = NULL;
  struct type *type1, *type2;

  switch (op)
    {
    /* Handle binary relational and equality operators that are either not
       or differently defined for GNU vectors.  */
    case BINOP_EQUAL:
    case BINOP_NOTEQUAL:
    case BINOP_LESS:
    case BINOP_GTR:
    case BINOP_GEQ:
    case BINOP_LEQ:
      (*pos)++;
      arg1 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
      arg2 = evaluate_subexp (value_type (arg1), exp, pos, noside);

      if (noside == EVAL_SKIP)
	return value_from_longest (builtin_type (exp->gdbarch)->
				   builtin_int, 1);

      return opencl_relop (exp, arg1, arg2, op);

    /* Handle the logical unary operator not(!).  */
    case UNOP_LOGICAL_NOT:
      (*pos)++;
      arg1 = evaluate_subexp (NULL_TYPE, exp, pos, noside);

      if (noside == EVAL_SKIP)
	return value_from_longest (builtin_type (exp->gdbarch)->
				   builtin_int, 1);

      return opencl_logical_not (exp, arg1);

    /* Handle the logical operator and(&&) and or(||).  */
    case BINOP_LOGICAL_AND:
    case BINOP_LOGICAL_OR:
      (*pos)++;
      arg1 = evaluate_subexp (NULL_TYPE, exp, pos, noside);

      if (noside == EVAL_SKIP)
	{
	  arg2 = evaluate_subexp (NULL_TYPE, exp, pos, noside);

	  return value_from_longest (builtin_type (exp->gdbarch)->
				     builtin_int, 1);
	}
      else
	{
	  /* For scalar operations we need to avoid evaluating operands
	     unecessarily.  However, for vector operations we always need to
	     evaluate both operands.  Unfortunately we only know which of the
	     two cases apply after we know the type of the second operand.
	     Therefore we evaluate it once using EVAL_AVOID_SIDE_EFFECTS.  */
	  int oldpos = *pos;

	  arg2 = evaluate_subexp (NULL_TYPE, exp, pos, EVAL_AVOID_SIDE_EFFECTS);
	  *pos = oldpos;
	  type1 = check_typedef (value_type (arg1));
	  type2 = check_typedef (value_type (arg2));

	  if ((TYPE_CODE (type1) == TYPE_CODE_ARRAY && TYPE_VECTOR (type1))
	      || (TYPE_CODE (type2) == TYPE_CODE_ARRAY && TYPE_VECTOR (type2)))
	    {
	      arg2 = evaluate_subexp (NULL_TYPE, exp, pos, noside);

	      return opencl_relop (exp, arg1, arg2, op);
	    }
	  else
	    {
	      /* For scalar built-in types, only evaluate the right
		 hand operand if the left hand operand compares
		 unequal(&&)/equal(||) to 0.  */
	      int res;
	      int tmp = value_logical_not (arg1);

	      if (op == BINOP_LOGICAL_OR)
		tmp = !tmp;

	      arg2 = evaluate_subexp (NULL_TYPE, exp, pos,
				      tmp ? EVAL_SKIP : noside);
	      type1 = language_bool_type (exp->language_defn, exp->gdbarch);

	      if (op == BINOP_LOGICAL_AND)
		res = !tmp && !value_logical_not (arg2);
	      else /* BINOP_LOGICAL_OR */
		res = tmp || !value_logical_not (arg2);

	      return value_from_longest (type1, res);
	    }
	}

    /* Handle the ternary selection operator.  */
    case TERNOP_COND:
      (*pos)++;
      arg1 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
      type1 = check_typedef (value_type (arg1));
      if (TYPE_CODE (type1) == TYPE_CODE_ARRAY && TYPE_VECTOR (type1))
	{
	  struct value *arg3, *tmp, *ret;
	  struct type *eltype2, *type3, *eltype3;
	  int t2_is_vec, t3_is_vec, i;
	  LONGEST lowb1, lowb2, lowb3, highb1, highb2, highb3;

	  arg2 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
	  arg3 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
	  type2 = check_typedef (value_type (arg2));
	  type3 = check_typedef (value_type (arg3));
	  t2_is_vec
	    = TYPE_CODE (type2) == TYPE_CODE_ARRAY && TYPE_VECTOR (type2);
	  t3_is_vec
	    = TYPE_CODE (type3) == TYPE_CODE_ARRAY && TYPE_VECTOR (type3);

	  /* Widen the scalar operand to a vector if necessary.  */
	  if (t2_is_vec || !t3_is_vec)
	    {
	      arg3 = value_cast (type2, arg3);
	      type3 = value_type (arg3);
	    }
	  else if (!t2_is_vec || t3_is_vec)
	    {
	      arg2 = value_cast (type3, arg2);
	      type2 = value_type (arg2);
	    }
	  else if (!t2_is_vec || !t3_is_vec)
	    {
	      /* Throw an error if arg2 or arg3 aren't vectors.  */
	      error (_("\
Cannot perform conditional operation on incompatible types"));
	    }

	  eltype2 = check_typedef (TYPE_TARGET_TYPE (type2));
	  eltype3 = check_typedef (TYPE_TARGET_TYPE (type3));

	  if (!get_array_bounds (type1, &lowb1, &highb1)
	      || !get_array_bounds (type2, &lowb2, &highb2)
	      || !get_array_bounds (type3, &lowb3, &highb3))
	    error (_("Could not determine the vector bounds"));

	  /* Throw an error if the types of arg2 or arg3 are incompatible.  */
	  if (TYPE_CODE (eltype2) != TYPE_CODE (eltype3)
	      || TYPE_LENGTH (eltype2) != TYPE_LENGTH (eltype3)
	      || TYPE_UNSIGNED (eltype2) != TYPE_UNSIGNED (eltype3)
	      || lowb2 != lowb3 || highb2 != highb3)
	    error (_("\
Cannot perform operation on vectors with different types"));

	  /* Throw an error if the sizes of arg1 and arg2/arg3 differ.  */
	  if (lowb1 != lowb2 || lowb1 != lowb3
	      || highb1 != highb2 || highb1 != highb3)
	    error (_("\
Cannot perform conditional operation on vectors with different sizes"));

	  ret = allocate_value (type2);

	  for (i = 0; i < highb1 - lowb1 + 1; i++)
	    {
	      tmp = value_logical_not (value_subscript (arg1, i)) ?
		    value_subscript (arg3, i) : value_subscript (arg2, i);
	      memcpy (value_contents_writeable (ret) +
		      i * TYPE_LENGTH (eltype2), value_contents_all (tmp),
		      TYPE_LENGTH (eltype2));
	    }

	  return ret;
	}
      else
	{
	  if (value_logical_not (arg1))
	    {
	      /* Skip the second operand.  */
	      evaluate_subexp (NULL_TYPE, exp, pos, EVAL_SKIP);

	      return evaluate_subexp (NULL_TYPE, exp, pos, noside);
	    }
	  else
	    {
	      /* Skip the third operand.  */
	      arg2 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
	      evaluate_subexp (NULL_TYPE, exp, pos, EVAL_SKIP);

	      return arg2;
	    }
	}

    /* Handle STRUCTOP_STRUCT to allow component access on OpenCL vectors.  */
    case STRUCTOP_STRUCT:
      {
	int pc = (*pos)++;
	int tem = longest_to_int (exp->elts[pc + 1].longconst);

	(*pos) += 3 + BYTES_TO_EXP_ELEM (tem + 1);
	arg1 = evaluate_subexp (NULL_TYPE, exp, pos, noside);
	type1 = check_typedef (value_type (arg1));

	if (noside == EVAL_SKIP)
	  {
	    return value_from_longest (builtin_type (exp->gdbarch)->
				       builtin_int, 1);
	  }
	else if (TYPE_CODE (type1) == TYPE_CODE_ARRAY && TYPE_VECTOR (type1))
	  {
	    return opencl_component_ref (exp, arg1, &exp->elts[pc + 2].string,
					 noside);
	  }
	else
	  {
	    if (noside == EVAL_AVOID_SIDE_EFFECTS)
	      return
		  value_zero (lookup_struct_elt_type
			      (value_type (arg1),&exp->elts[pc + 2].string, 0),
			      lval_memory);
	    else
	      return value_struct_elt (&arg1, NULL,
				       &exp->elts[pc + 2].string, NULL,
				       "structure");
	  }
      }
    default:
      break;
    }

  return evaluate_subexp_c (expect_type, exp, pos, noside);
}

void
opencl_language_arch_info (struct gdbarch *gdbarch,
		      struct language_arch_info *lai)
{
  const struct builtin_opencl_type *builtin = builtin_opencl_type (gdbarch);

  lai->string_char_type = builtin->builtin_char;
  lai->primitive_type_vector
    = GDBARCH_OBSTACK_CALLOC (gdbarch, nr_opencl_primitive_types + 1,
			      struct type *);

/* This macro fills the primitive_type_vector from a given type.  */
#define FILL_TYPE_VECTOR(LAI, TYPE)\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE]\
    = builtin->builtin_##TYPE;\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE##2]\
    = builtin->builtin_##TYPE##2;\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE##3]\
    = builtin->builtin_##TYPE##3;\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE##4]\
    = builtin->builtin_##TYPE##4;\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE##8]\
    = builtin->builtin_##TYPE##8;\
  LAI->primitive_type_vector [opencl_primitive_type_##TYPE##16]\
    = builtin->builtin_##TYPE##16

  FILL_TYPE_VECTOR (lai, char);
  FILL_TYPE_VECTOR (lai, uchar);
  FILL_TYPE_VECTOR (lai, short);
  FILL_TYPE_VECTOR (lai, ushort);
  FILL_TYPE_VECTOR (lai, int);
  FILL_TYPE_VECTOR (lai, uint);
  FILL_TYPE_VECTOR (lai, long);
  FILL_TYPE_VECTOR (lai, ulong);
  FILL_TYPE_VECTOR (lai, half);
  FILL_TYPE_VECTOR (lai, float);
  FILL_TYPE_VECTOR (lai, double);
  lai->primitive_type_vector [opencl_primitive_type_bool]
    = builtin->builtin_bool;
  lai->primitive_type_vector [opencl_primitive_type_unsigned_char]
    = builtin->builtin_unsigned_char;
  lai->primitive_type_vector [opencl_primitive_type_unsigned_short]
    = builtin->builtin_unsigned_short;
  lai->primitive_type_vector [opencl_primitive_type_unsigned_int]
    = builtin->builtin_unsigned_int;
  lai->primitive_type_vector [opencl_primitive_type_unsigned_long]
    = builtin->builtin_unsigned_long;
  lai->primitive_type_vector [opencl_primitive_type_half]
    = builtin->builtin_half;
  lai->primitive_type_vector [opencl_primitive_type_size_t]
    = builtin->builtin_size_t;
  lai->primitive_type_vector [opencl_primitive_type_ptrdiff_t]
    = builtin->builtin_ptrdiff_t;
  lai->primitive_type_vector [opencl_primitive_type_intptr_t]
    = builtin->builtin_intptr_t;
  lai->primitive_type_vector [opencl_primitive_type_uintptr_t]
    = builtin->builtin_uintptr_t;
  lai->primitive_type_vector [opencl_primitive_type_void]
    = builtin->builtin_void;

  /* Specifies the return type of logical and relational operations.  */
  lai->bool_type_symbol = "int";
  lai->bool_type_default = builtin->builtin_int;
}

const struct exp_descriptor exp_descriptor_opencl =
{
  print_subexp_standard,
  operator_length_standard,
  operator_check_standard,
  op_name_standard,
  dump_subexp_body_standard,
  evaluate_subexp_opencl
};

const struct language_defn opencl_language_defn =
{
  "opencl",			/* Language name */
  language_opencl,
  range_check_off,
  type_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  &exp_descriptor_opencl,
  c_parse,
  c_error,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  NULL,				/* Language specific skip_trampoline */
  NULL,                         /* name_of_this */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,				/* Language specific class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_make_symbol_completion_list,
  opencl_language_arch_info,
  default_print_array_index,
  default_pass_by_reference,
  c_get_string,
  LANG_MAGIC
};

static void *
build_opencl_types (struct gdbarch *gdbarch)
{
  struct builtin_opencl_type *builtin_opencl_type
    = GDBARCH_OBSTACK_ZALLOC (gdbarch, struct builtin_opencl_type);

/* Helper macro to create strings.  */
#define STRINGIFY(S) #S
/* This macro allocates and assigns the type struct pointers
   for the vector types.  */
#define BUILD_OCL_VTYPES(TYPE)\
  builtin_opencl_type->builtin_##TYPE##2\
    = init_vector_type (builtin_opencl_type->builtin_##TYPE, 2);\
  TYPE_NAME (builtin_opencl_type->builtin_##TYPE##2) = STRINGIFY(TYPE ## 2);\
  builtin_opencl_type->builtin_##TYPE##3\
    = init_vector_type (builtin_opencl_type->builtin_##TYPE, 3);\
  TYPE_NAME (builtin_opencl_type->builtin_##TYPE##3) = STRINGIFY(TYPE ## 3);\
  TYPE_LENGTH (builtin_opencl_type->builtin_##TYPE##3)\
    = 4 * TYPE_LENGTH (builtin_opencl_type->builtin_##TYPE);\
  builtin_opencl_type->builtin_##TYPE##4\
    = init_vector_type (builtin_opencl_type->builtin_##TYPE, 4);\
  TYPE_NAME (builtin_opencl_type->builtin_##TYPE##4) = STRINGIFY(TYPE ## 4);\
  builtin_opencl_type->builtin_##TYPE##8\
    = init_vector_type (builtin_opencl_type->builtin_##TYPE, 8);\
  TYPE_NAME (builtin_opencl_type->builtin_##TYPE##8) = STRINGIFY(TYPE ## 8);\
  builtin_opencl_type->builtin_##TYPE##16\
    = init_vector_type (builtin_opencl_type->builtin_##TYPE, 16);\
  TYPE_NAME (builtin_opencl_type->builtin_##TYPE##16) = STRINGIFY(TYPE ## 16)

  builtin_opencl_type->builtin_char
    = arch_integer_type (gdbarch, 8, 0, "char");
  BUILD_OCL_VTYPES (char);
  builtin_opencl_type->builtin_uchar
    = arch_integer_type (gdbarch, 8, 1, "uchar");
  BUILD_OCL_VTYPES (uchar);
  builtin_opencl_type->builtin_short
    = arch_integer_type (gdbarch, 16, 0, "short");
  BUILD_OCL_VTYPES (short);
  builtin_opencl_type->builtin_ushort
    = arch_integer_type (gdbarch, 16, 1, "ushort");
  BUILD_OCL_VTYPES (ushort);
  builtin_opencl_type->builtin_int
    = arch_integer_type (gdbarch, 32, 0, "int");
  BUILD_OCL_VTYPES (int);
  builtin_opencl_type->builtin_uint
    = arch_integer_type (gdbarch, 32, 1, "uint");
  BUILD_OCL_VTYPES (uint);
  builtin_opencl_type->builtin_long
    = arch_integer_type (gdbarch, 64, 0, "long");
  BUILD_OCL_VTYPES (long);
  builtin_opencl_type->builtin_ulong
    = arch_integer_type (gdbarch, 64, 1, "ulong");
  BUILD_OCL_VTYPES (ulong);
  builtin_opencl_type->builtin_half
    = arch_float_type (gdbarch, 16, "half", floatformats_ieee_half);
  BUILD_OCL_VTYPES (half);
  builtin_opencl_type->builtin_float
    = arch_float_type (gdbarch, 32, "float", floatformats_ieee_single);
  BUILD_OCL_VTYPES (float);
  builtin_opencl_type->builtin_double
    = arch_float_type (gdbarch, 64, "double", floatformats_ieee_double);
  BUILD_OCL_VTYPES (double);
  builtin_opencl_type->builtin_bool
    = arch_boolean_type (gdbarch, 32, 1, "bool");
  builtin_opencl_type->builtin_unsigned_char
    = arch_integer_type (gdbarch, 8, 1, "unsigned char");
  builtin_opencl_type->builtin_unsigned_short
    = arch_integer_type (gdbarch, 16, 1, "unsigned short");
  builtin_opencl_type->builtin_unsigned_int
    = arch_integer_type (gdbarch, 32, 1, "unsigned int");
  builtin_opencl_type->builtin_unsigned_long
    = arch_integer_type (gdbarch, 64, 1, "unsigned long");
  builtin_opencl_type->builtin_size_t
    = arch_integer_type (gdbarch, gdbarch_ptr_bit (gdbarch), 1, "size_t");
  builtin_opencl_type->builtin_ptrdiff_t
    = arch_integer_type (gdbarch, gdbarch_ptr_bit (gdbarch), 0, "ptrdiff_t");
  builtin_opencl_type->builtin_intptr_t
    = arch_integer_type (gdbarch, gdbarch_ptr_bit (gdbarch), 0, "intptr_t");
  builtin_opencl_type->builtin_uintptr_t
    = arch_integer_type (gdbarch, gdbarch_ptr_bit (gdbarch), 1, "uintptr_t");
  builtin_opencl_type->builtin_void
    = arch_type (gdbarch, TYPE_CODE_VOID, 1, "void");

  return builtin_opencl_type;
}

void
_initialize_opencl_language (void)
{
  opencl_type_data = gdbarch_data_register_post_init (build_opencl_types);
  add_language (&opencl_language_defn);
}