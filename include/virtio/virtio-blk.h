/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_VIRTIO_BLK_H
#define RELM_VIRTIO_BLK_H
 
#include <linux/types.h>
#include <linux/file.h>
 `:wq
`
#include <include/virtio/virtio.h>

struct relm_vm; 


/*request types */ 
#define VIRTIO_BLK_T_IN     0 /*read from disk to guest buffer */ 
#define VIRTIO_BLK_T_OUT    1 /*write from guest buffer to disk */ 
#define VIRTIO_BLK_T_FLUSH  2 /*flush write chache to backin store */ 

/*status byte values */ 
#define VIRTIO_BLK_T_OK     0
#define VIRTIO_BLK_S_IOERR  1 
#define VIRTIO_BLK_T_UNSUPP 2 


struct virtio_blk_req{
    __le32 types; 
    __le32 reserved; 
    __le64 sector; 
}__attribute__((packed)); 


struct virtio_blk_config{
    __le64 capacity; 
}
