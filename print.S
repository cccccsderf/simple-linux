TI_GDT equ 0
RPL0   equ 0
SELECTOR_VIDEO equ (0x003<<3) + TI_GDT + RPL0

[bits 32]
section .text
section .data
put_int_buffer dq 0

global put_str
put_str:
    push ebx
    push ecx
    xor ecx, ecx
    mov ebx, [esp + 12]
.goon:
    mov cl, [ebx]
    cmp cl, 0
    jz .str_over
    push ecx
    call put_char
    add esp, 4
    inc ebx
    jmp .goon
.str_over:
    pop ecx
    pop ebx
    ret

global put_int
put_int:
    pushad
    mov ebp, esp
    mov eax, [ebp + 4*9] ;call的返回地址占4个字节
    mov edx, eax
    mov edi, 7           ;指定在put_int_buffer中初始的偏移量
    mov ecx, 8           ;32位中，十六进制数字的位数是8个
    mov ebx, put_int_buffer
.16based_4bits:
    and edx, 0x0000000F
    cmp edx, 9
    jg .is_A2F
    add edx, '0'
    jmp .store
.is_A2F:
    sub edx, 10

    add edx, 'A'

.store:
    mov [ebx+edi], dl
    dec edi
    shr eax, 4
    mov edx, eax
    loop .16based_4bits

.ready_to_print:
    inc edi
.skip_prefix_0:
    cmp edi, 8

    je .full0
.go_on_skip:
    mov cl, [put_int_buffer + edi]
    inc edi
    cmp cl, '0'
    je .skip_prefix_0
    dec edi
    jmp .put_each_num

.full0:
    mov cl, '0'
.put_each_num:
    push ecx
    call put_char
    add esp, 4
    inc edi
    mov cl, [put_int_buffer + edi]
    cmp edi, 8
    jl .put_each_num
    popad
    ret

;把栈中的1个字符写入光标所在处
global put_char
put_char:
    pushad      ;push all double 备份32位寄存器环境
                ;需要保证gs中为正确的视频段选择子
        ;为保险起见，每次打印是都为gs赋值
    mov ax, SELECTOR_VIDEO
    mov gs, ax

    ;获取当前光标位置
    ;先获取高8位
    mov dx, 0x03d4      ;索引寄存器
    mov al, 0x0e        ;用于提供光标位置的高8位
    out dx, al
    mov dx, 0x03d5      ;通过读写数据端口   0x03d5获得或设置光标位置
    in  al, dx          ;得到光标位置的高8位
    mov ah, al

    ;在获取低8位
    mov dx, 0x03d4
    mov al, 0x0f
    out dx, al
    mov dx, 0x03d5
    in  al, dx

    ;将光标存入bx
    mov bx, ax
    ;下面这行是在栈中获取待打印的字符
    mov ecx, [esp + 36] ;pushad压入4x8=32字节  加上主调函数4字节的返回地址，故esp + 36
    cmp cl, 0xd         ;CR是0xd,LF是0x0a
    jz .is_carriage_return
    cmp cl, 0xa
    jz .is_line_feed

    cmp cl, 0x8         ;BS(backspace)的ASCII吗是8
    jz .is_backspace
    jmp .put_other

    .is_backspace:
    dec bx
    shl bx, 1           ;光标左移一位等于乘2
                        ;表示光标对应显存中的偏移字节
    mov byte [gs:bx], 0x20 ;将待删除的字节补位0或者空格皆可
    inc bx
    mov byte [gs:bx], 0x07
    shr bx, 1
    jmp .set_cursor

    .put_other:
    shl bx, 1           ;光标位置用2字节表示，将光标值乘2
                        ;表示光标对应显存中的偏移字节
    mov [gs:bx], cl
    inc bx
    mov byte [gs:bx], 0x07 ;字符属性
    shr bx, 1           ;恢复老的光标值
    inc bx
    cmp bx, 2000
    jl .set_cursor      ;若光标值小于2000，表示未写到
                        ;显存的最后，则区设置新的光标值
                        ;若超出屏幕字符数大小2000
                        ;则换行处理
    .is_line_feed:      ;是换行符LF（\n）
    .is_carriage_return: ;是回车符CR（\r）
    ;如果是CR，只要把光标移到行首就行了
    xor dx, dx          ;dx是被除数的高16位，清0
    mov ax, bx          ;ax是被除数的低16位
    mov si, 80          ;由于是效仿linux中的\n表示下一行的行首，所以\n和\r都处理为\n

    div si              ;\n和\r都处理为\n,也就是下一行的行首

    sub bx, dx          ;光标值减去除80的余数便是取整
                        ;以上四行处理\r的代码
    .is_carriage_return_end:   ;回车符CR处理结束
    add bx, 80
    cmp bx, 2000
    .is_line_feed_end:         ;若是LF（\n）,将光标移+80便可
    jl .set_cursor

    .roll_screen:       ;若超出屏幕大小，开始滚屏
    cld
    mov ecx, 960        ;2000-80=1920个字符要搬运，共1920*2=3840字节
                        ;一次搬4字节，共3840/4=960次
    mov esi, 0xc00b80a0 ;第一行行首
    mov edi, 0xc00b8000 ;第0行行首
    rep movsd

    ;将最后一行填充为空白
    mov ebx, 3840       ;最后一行首字符的第一个字节编译=1920*2
    mov ecx, 80         ;一行是80字符（160字节），每次清空1字符(2字节)，一行需要移动80次
    
    .cls:
    mov word [gs:ebx], 0x0720 ;0x0720是黑底白字的空格键
    add ebx, 2
    loop .cls
    mov bx, 1920        ;将光标值重置为1920，最后一行的首字符

    .set_cursor:
    ;将光标设为bx值
    ;;;;;先设置高8位
    mov dx, 0x03d4      ;索引寄存器
    mov al, 0x0e        ;用户提供光标位置的高8位
    out dx, al
    mov dx, 0x03d5      ;通过读写数据端口0x03d5获取或设置光标位置
    mov al, bh
    out dx, al

    ;再设置低8位
    mov dx, 0x03d4
    mov al, 0x0f
    out dx, al
    mov dx, 0x03d5
    mov al, bl
    out dx, al
    .put_char_done:
    popad
    ret

    global cls_screen
cls_screen:
   pushad
   ;;;;;;;;;;;;;;;
	; 由于用户程序的cpl为3,显存段的dpl为0,故用于显存段的选择子gs在低于自己特权的环境中为0,
	; 导致用户程序再次进入中断后,gs为0,故直接在put_str中每次都为gs赋值. 
   mov ax, SELECTOR_VIDEO	       ; 不能直接把立即数送入gs,须由ax中转
   mov gs, ax

   mov ebx, 0
   mov ecx, 80*25
 .cls:
   mov word [gs:ebx], 0x0720		  ;0x0720是黑底白字的空格键
   add ebx, 2
   loop .cls 
   mov ebx, 0

 .set_cursor:				  ;直接把set_cursor搬过来用,省事
;;;;;;; 1 先设置高8位 ;;;;;;;;
   mov dx, 0x03d4			  ;索引寄存器
   mov al, 0x0e				  ;用于提供光标位置的高8位
   out dx, al
   mov dx, 0x03d5			  ;通过读写数据端口0x3d5来获得或设置光标位置 
   mov al, bh
   out dx, al

;;;;;;; 2 再设置低8位 ;;;;;;;;;
   mov dx, 0x03d4
   mov al, 0x0f
   out dx, al
   mov dx, 0x03d5 
   mov al, bl
   out dx, al
   popad
   ret

    global set_cursor
    set_cursor:
    pushad
    mov bx, [esp+36]
    ;将光标设为bx值
    ;;;;;先设置高8位
    mov dx, 0x03d4      ;索引寄存器
    mov al, 0x0e        ;用户提供光标位置的高8位
    out dx, al
    mov dx, 0x03d5      ;通过读写数据端口0x03d5获取或设置光标位置
    mov al, bh
    out dx, al

    ;再设置低8位
    mov dx, 0x03d4
    mov al, 0x0f
    out dx, al
    mov dx, 0x03d5
    mov al, bl
    out dx, al
    .put_char_done:
    popad
    ret