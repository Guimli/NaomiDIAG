! NetDIMM firmware blob of a LOCAL build (make ... DIMMFW=1): SEGA
! code packed from the owner's own dumps by tools/dimmfw_pack.py into
! local/dimmfw.bin (git-ignored). Without DIMMFW the blob is empty and the
! firmware-update action can only identify the flash.
    .section .rodata
    .balign 4
    .global _dimmfw_blob
_dimmfw_blob:
#if DIMMFW
    .incbin "local/dimmfw.bin"
#endif
    .balign 4
    .global _dimmfw_blob_end
_dimmfw_blob_end:
