#ifndef __ARK_SERVICE_H__
#define __ARK_SERVICE_H__

/* 已知一个结构体里面的成员的地址，反推出该结构体的首地址 */
#define ark_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))
			
#define ark_list_entry(node, type, member) \
    ark_container_of(node, type, member)
		

ark_inline void ark_list_init(ark_list_t *l)
{
    l->next = l->prev = l;
}

/* 在双向链表头部插入一个节点 */
ark_inline void ark_list_insert_after(ark_list_t *l, ark_list_t *n)
{
    l->next->prev = n;
    n->next = l->next;

    l->next = n;
    n->prev = l;
}

/* 在双向链表尾部插入一个节点 */
ark_inline void ark_list_insert_before(ark_list_t *l, ark_list_t *n)
{
    l->prev->next = n;
    n->prev = l->prev;

    l->prev = n;
    n->next = l;
}

/* 从双向链表删除一个节点 */
ark_inline void ark_list_remove(ark_list_t *n)
{
    n->next->prev = n->prev;
    n->prev->next = n->next;

    n->next = n->prev = n;
}

#endif
