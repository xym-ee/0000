#include "dbus.h"

/* 遥控器按键全局数据 */
dbus_t dbus = {
    .rv = RC_CH_VALUE_OFFSET,
    .rh = RC_CH_VALUE_OFFSET,
    .lv = RC_CH_VALUE_OFFSET,
    .lh = RC_CH_VALUE_OFFSET,
    .sl  = RC_SW_MID,
    .sr  = RC_SW_MID,
};


/*--------------------------  控制块  ---------------------------*/

/* 串口设备句柄 */
static rt_device_t serial;

/* 串口接收消息结构*/
struct rx_msg
{
    rt_device_t dev;
    rt_size_t size;
};


/* 消息队列控制块 */
static struct rt_messagequeue rx_mq;


/*--------------------------  DMA中断回调  ---------------------------*/

/* 接收数据回调函数 */
static rt_err_t uart_input(rt_device_t dev, rt_size_t size)
{
    struct rx_msg msg;
    rt_err_t result;
    msg.dev = dev;
    msg.size = size;

    result = rt_mq_send(&rx_mq, &msg, sizeof(msg));
    if ( result == -RT_EFULL)
    {
        /* 消息队列满 */
        rt_kprintf("message queue full！\n");
    }
    return result;
}

/*--------------------------  数据处理线程  ---------------------------*/

/*
    遥控器摇杆收到的数据范围 364-1684  中位 1024
*/

static void dbus_thread_entry(void *parameter)
{
    struct rx_msg msg;
    rt_err_t result;
    rt_uint32_t rx_length;
    static char rx_buffer[RT_SERIAL_RB_BUFSZ + 1];

    while (1)
    {
        rt_memset(&msg, 0, sizeof(msg));
        /* 从消息队列中读取消息*/
        result = rt_mq_recv(&rx_mq, &msg, sizeof(msg), RT_WAITING_FOREVER);
        if (result == RT_EOK)
        {
            /* 从串口读取数据*/
            rx_length = rt_device_read(msg.dev, 0, rx_buffer, msg.size);
            
            if (rx_length == RC_FRAME_LENGTH)        /* 长度正确，解析遥控器数据 */
            {
                dbus.rh             = ((int16_t)rx_buffer[0] | ((int16_t)rx_buffer[1] << 8)) & 0x07FF;
                dbus.rv             = (((int16_t)rx_buffer[1] >> 3) | ((int16_t)rx_buffer[2] << 5)) & 0x07FF;
                dbus.lh             = (((int16_t)rx_buffer[2] >> 6) | ((int16_t)rx_buffer[3] << 2) | ((int16_t)rx_buffer[4] << 10)) & 0x07FF;
                dbus.lv             = (((int16_t)rx_buffer[4] >> 1) | ((int16_t)rx_buffer[5]<<7)) & 0x07FF;
                dbus.sl             = ((rx_buffer[5] >> 4) & 0x000C) >> 2;
                dbus.sr             = ((rx_buffer[5] >> 4) & 0x0003);
                dbus.mouse.x        = ((int16_t)rx_buffer[6]) | ((int16_t)rx_buffer[7] << 8);
                dbus.mouse.y        = ((int16_t)rx_buffer[8]) | ((int16_t)rx_buffer[9] << 8);
                dbus.mouse.z        = ((int16_t)rx_buffer[10]) | ((int16_t)rx_buffer[11] << 8);
                dbus.mouse.press_l  = rx_buffer[12];
                dbus.mouse.press_r  = rx_buffer[13];
                dbus.key.v          = ((int16_t)rx_buffer[14]);// | ((int16_t)rx_buffer[15] << 8);
            }
        }
    }
}

int dbus_init(void)
{
    rt_err_t ret = RT_EOK;
    static char msg_pool[256];
    
    /* dr16的初始化配置参数 */
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;  
    config.baud_rate = 100000;
    config.data_bits = DATA_BITS_8;
    config.parity    = PARITY_EVEN;
    config.stop_bits = STOP_BITS_1;
    
    /* 查找串口设备 */
    serial = rt_device_find(DBUS_UART);
    if (!serial)
    {
        rt_kprintf("find %s failed!\n", DBUS_UART);
        return RT_ERROR;
    }

    /* 初始化消息队列 */
    rt_mq_init(&rx_mq, "rx_mq",
               msg_pool,                 /* 存放消息的缓冲区 */
               sizeof(struct rx_msg),    /* 一条消息的最大长度 */
               sizeof(msg_pool),         /* 存放消息的缓冲区大小 */
               RT_IPC_FLAG_FIFO);        /* 如果有多个线程等待，按照先来先得到的方法分配消息 */

    
    rt_device_control(serial, RT_DEVICE_CTRL_CONFIG, &config);      
    
    /* 以 DMA 接收及轮询发送方式打开串口设备 */
    rt_device_open(serial, RT_DEVICE_FLAG_DMA_RX);
    
               /* 设置接收回调函数 */
    rt_device_set_rx_indicate(serial, uart_input);

    /* 创建 serial 线程 */
    rt_thread_t thread = rt_thread_create("dbus",
                                          dbus_thread_entry,
                                          RT_NULL,
                                          DBUS_THREAD_STACK_SIZE,
                                          DBUS_THREAD_PRIORITY,
                                          DBUS_THREAD_TIMESLICE);
    
    /* 创建成功则启动线程 */
    if (thread != RT_NULL)
    {
        rt_thread_startup(thread);
    }
    else
    {
        ret = RT_ERROR;
    }

    return ret;
}

INIT_APP_EXPORT(dbus_init);


/*--------------------------  调试输出线程  ---------------------------*/

static void dbus_debug_thread_entry(void *parameter)
{
    while (1)
    {
        rt_kprintf("%4d %4d\n", dbus.lv, dbus.rv);
        rt_kprintf("%4d %4d\n", dbus.lh, dbus.rh);
        rt_kprintf("%4d %4d\n", dbus.sl, dbus.sr);
        
//        rt_kprintf("%d \n", dr16.mouse.x);
//        rt_kprintf("%d \n", dr16.mouse.y);
//        rt_kprintf("%d \n", dr16.mouse.z);
//        rt_kprintf("%d \n", dr16.mouse.press_l);
//        rt_kprintf("%d \n", dr16.mouse.press_r);
//        rt_kprintf("%d \n", dr16.key.v);
        
        rt_thread_mdelay(500);
    }
}

static int dbus_output(int argc, char *argv[])
{
    rt_thread_t thread = rt_thread_create("dbus_output", dbus_debug_thread_entry, RT_NULL, 1024, 25, 10);
    /* 创建成功则启动线程 */
    if (thread != RT_NULL)
    {
        rt_thread_startup(thread);
    }

    return 0;
}
/* 导出到 msh 命令列表中 */
MSH_CMD_EXPORT(dbus_output, remote controller data debug outputuart);

