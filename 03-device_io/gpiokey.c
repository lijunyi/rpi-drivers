#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/poll.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Philon | https://ixx.life");

#define KEY_GPIO  17

static struct {
  int irq;                  // 按键GPIO中断号
  struct timer_list delay;  // 防抖延时
  wait_queue_head_t r_wait; // IO阻塞等待队列
  struct fasync_struct* fa; // 异步IO
} dev;

static int gpiokey_open(struct inode *node, struct file *filp)
{
  return 0;
}

static __poll_t gpiokey_poll(struct file *filp, struct poll_table_struct *wait)
{
  __poll_t mask = 0;
  
  // 加入等待队列
  poll_wait(filp, &dev.r_wait, wait);
  if (gpio_get_value(KEY_GPIO)) {
    mask = POLLIN | POLLRDNORM;
  }

  return mask;
}

static ssize_t gpiokey_read(struct file *filp, char __user *buf, size_t len, loff_t * off)
{
  int data = 1;
  DECLARE_WAITQUEUE(wait, current);
  add_wait_queue(&dev.r_wait, &wait);

  if ((filp->f_flags & O_NONBLOCK) && !gpio_get_value(KEY_GPIO)) {
    // 如果是非阻塞访问，且按键没有按下时，直接返回错误
    return -EAGAIN;
  }

  // 等待事件唤醒(可被信号中断)，进程进入阻塞状态
  wait_event_interruptible(dev.r_wait, gpio_get_value(KEY_GPIO));
  remove_wait_queue(&dev.r_wait, &wait);

  // 被唤醒后，进行业务处理，返回一个“按下”标志给用户进程
  len = sizeof(data);
  if ((data = copy_to_user(buf, &data, len)) < 0) {
    return -EFAULT;
  }
  *off += len;

  return 0;
}

static int gpiokey_fasync(int fd, struct file *filp, int mode)
{
  return fasync_helper(fd, filp, mode, &dev.fa);
}

static int gpiokey_close(struct inode *node, struct file *filp)
{
  gpiokey_fasync(-1, filp, 0);
  return 0;
}

static long gpiokey_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
  return 0;
}

// 按键中断顶半部响应及防抖延时判断
static irqreturn_t on_key_pressed(int irq, void* dev_id)
{
  mod_timer(&dev.delay, jiffies + (HZ/20));
  return IRQ_HANDLED;
}
static void on_delay50(struct timer_list* timer)
{
  if (gpio_get_value(KEY_GPIO)) {
    // 按下按键后，唤醒阻塞队列
    wake_up_interruptible(&dev.r_wait);
    kill_fasync(&dev.fa, SIGIO, POLL_IN);
  }
}

struct file_operations fops = {
  .owner = THIS_MODULE,
  .open = gpiokey_open,
  .release = gpiokey_close,
  .read = gpiokey_read,
  .poll = gpiokey_poll,
  .fasync = gpiokey_fasync,
  .unlocked_ioctl = gpiokey_ioctl,
};

struct miscdevice gpiokey = {
  .minor = 1,
  .name = "gpiokey",
  .fops = &fops,
  .nodename = "mykey",
  .mode = 0744,
};

static int __init gpiokey_init(void)
{
  // 初始化定时器，用于防抖延时
  timer_setup(&dev.delay, on_delay50, 0);
  add_timer(&dev.delay);

  // 初始化“读阻塞”等待队列
  init_waitqueue_head(&dev.r_wait);

  // 向内核申请GPIO和IRQ并绑定中断处理函数
  gpio_request_one(KEY_GPIO, GPIOF_IN, "key");
  dev.irq = gpio_to_irq(KEY_GPIO);
  if (request_irq(dev.irq, on_key_pressed, IRQF_TRIGGER_RISING, "onKeyPress", NULL) < 0) {
    printk(KERN_ERR "Failed to request irq for gpio%d\n", KEY_GPIO);
  }

  // 注册驱动模块并创建设备节点
  misc_register(&gpiokey);
  return 0;
}
module_init(gpiokey_init);

static void __exit gpiokey_exit(void)
{
  misc_deregister(&gpiokey);
  free_irq(dev.irq, NULL);
  gpio_free(KEY_GPIO);
  del_timer(&dev.delay);
}
module_exit(gpiokey_exit);