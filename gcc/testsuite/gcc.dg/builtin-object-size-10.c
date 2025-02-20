/* { dg-do compile } */
/* { dg-options "-O2 -fdump-tree-objsz-details" } */

typedef struct {
    char sentinel[4];
    char data[0];
} drone_packet;
typedef struct {
    char type_str[16];
    char channel_hop;
} __attribute__((packed)) drone_source_packet;
drone_packet *
foo(char *x)
{
  drone_packet *dpkt = __builtin_malloc(sizeof(drone_packet)
					+ sizeof(drone_source_packet));
  drone_source_packet *spkt = (drone_source_packet *) dpkt->data;
  __builtin___snprintf_chk (spkt->type_str, 16,
			    1, __builtin_object_size (spkt->type_str, 1),
			    "%s", x);
  return dpkt;
}

/* { dg-final { scan-tree-dump "maximum object size 21" "objsz" } } */
/* { dg-final { scan-tree-dump "maximum subobject size 16" "objsz" } } */
/* { dg-final { cleanup-tree-dump "objsz" } } */
