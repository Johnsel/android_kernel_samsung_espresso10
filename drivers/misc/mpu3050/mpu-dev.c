/*
    mpu-dev.c - mpu3050 char device interface

    Copyright (C) 1995-97 Simon G. Vogl
    Copyright (C) 1998-99 Frodo Looijaard <frodol@dds.nl>
    Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
    Copyright (C) 2010 InvenSense Corporation, All Rights Reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/* Code inside mpudev_ioctl_rdrw is copied from i2c-dev.c
 */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/signal.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/pm.h>

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/sensors_core.h>

#include "mpuirq.h"
#include "slaveirq.h"
#include "mlsl.h"
#include "mlos.h"
#include "mpu-i2c.h"
#include "mldl_cfg.h"
#include "mpu-accel.h"

#include "mpu_v333.h"

/* Platform data for the MPU */
struct mpu_private_data {
	struct mldl_cfg mldl_cfg;
	struct mutex power_lock;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	struct i2c_client *this_client;
	struct acc_data cal_data;
	int pid;
};

static struct mpu_private_data *mpu_private_data;

int read_accel_raw_xyz(struct mpu_private_data *mpu, struct acc_data *acc)
{
	unsigned char acc_data[6];
	s32 temp;

	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;

	if (mldl_cfg->accel_is_suspended == 1 ||
	    (mldl_cfg->dmp_is_running == 0
	     && mldl_cfg->accel_is_suspended == 0)) {
		if (sensor_i2c_read
		    (mpu->this_client->adapter, 0x0F, 0x06, 6, acc_data) != 0) {
			return -1;
		}
	} else if (mldl_cfg->dmp_is_running &&
		   mldl_cfg->accel_is_suspended == 0) {

		if (sensor_i2c_read
		    (mpu->this_client->adapter, DEFAULT_MPU_SLAVEADDR, 0x23, 6,
		     acc_data) != 0) {
			return -1;
		}
	} else
		return -1;

	temp = ((acc_data[1] << 4) | (acc_data[0] >> 4));
	if (temp < 2048)
		acc->x = (s16) (-temp);
	else
		acc->x = (s16) (4096 - temp);

	temp = ((acc_data[3] << 4) | (acc_data[2] >> 4));
	if (temp < 2048)
		acc->y = (s16) (-temp);
	else
		acc->y = (s16) (4096 - temp);

	temp = ((acc_data[5] << 4) | (acc_data[4] >> 4));
	acc->z = (s16) (3072 - temp);

	return 0;
}

static int accel_open_calibration(struct mpu_private_data *mpu)
{
	struct file *cal_filp;
	int err;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open("/efs/calibration_data", O_RDONLY, 0666);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		return err;
	}

	err = cal_filp->f_op->read(cal_filp,
				   (char *)&mpu->cal_data, 3 * sizeof(s16),
				   &cal_filp->f_pos);
	if (err != 3 * sizeof(s16)) {
		pr_err("%s: Can't read the cal data from file\n", __func__);
		err = -EIO;
	}

	pr_info("%s: (%u,%u,%u)\n", __func__,
	       mpu->cal_data.x, mpu->cal_data.y, mpu->cal_data.z);

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	return err;
}

static int accel_do_calibrate(struct mpu_private_data *mpu,
	unsigned long enable)
{
	struct acc_data data;
	struct file *cal_filp;
	int sum[3] = { 0, };
	int err;
	int i;
	mm_segment_t old_fs;

	for (i = 0; i < 100; i++) {
		err = read_accel_raw_xyz(mpu, &data);
		if (err < 0) {
			pr_err("%s: accel_read_accel_raw_xyz() "
			       "failed in the %dth loop\n", __func__, i);
			return err;
		}

		sum[0] += data.x;
		sum[1] += data.y;
		sum[2] += data.z;
	}

	if (enable) {
		mpu->cal_data.x = sum[0] / 100;
		mpu->cal_data.y = sum[1] / 100;
		mpu->cal_data.z = sum[2] / 100;
	} else {
		mpu->cal_data.x = 0;
		mpu->cal_data.y = 0;
		mpu->cal_data.z = 0;
	}

	pr_info("%s: cal data (%d,%d,%d)\n", __func__,
	       mpu->cal_data.x, mpu->cal_data.y, mpu->cal_data.z);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open("/efs/calibration_data",
			     O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		return err;
	}

	err = cal_filp->f_op->write(cal_filp,
				    (char *)&mpu->cal_data, 3 * sizeof(s16),
				    &cal_filp->f_pos);
	if (err != 3 * sizeof(s16)) {
		pr_err("%s: Can't write the cal data to file\n", __func__);
		err = -EIO;
	}

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	return err;
}

static int mpu_open(struct inode *inode, struct file *file)
{
	struct mldl_cfg *mldl_cfg = &mpu_private_data->mldl_cfg;
	accel_open_calibration(mpu_private_data);

	dev_dbg(&mpu_private_data->this_client->adapter->dev, "mpu_open\n");
	dev_dbg(&mpu_private_data->this_client->adapter->dev,
		"current->pid %d\n", current->pid);
	mpu_private_data->pid = current->pid;
	file->private_data = mpu_private_data->this_client;
	/* we could do some checking on the flags supplied by "open" */
	/* i.e. O_NONBLOCK */
	/* -> set some flag to disable interruptible_sleep_on in mpu_read */

	/* Reset the sensors to the default */
	mldl_cfg->requested_sensors = ML_THREE_AXIS_GYRO;
	if (mldl_cfg->accel && mldl_cfg->accel->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_ACCEL;

	if (mldl_cfg->compass && mldl_cfg->compass->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_COMPASS;

	if (mldl_cfg->pressure && mldl_cfg->pressure->resume)
		mldl_cfg->requested_sensors |= ML_THREE_AXIS_PRESSURE;

	return 0;
}

/* close function - called when the "file" /dev/mpu is closed in userspace   */
static int mpu_release(struct inode *inode, struct file *file)
{
	struct i2c_client *client = (struct i2c_client *)file->private_data;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;
	int result = 0;

	mpu->pid = 0;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);
	result = mpu3050_suspend(mldl_cfg, client->adapter,
				 accel_adapter, compass_adapter,
				 pressure_adapter, TRUE, TRUE, TRUE, TRUE);

	dev_dbg(&mpu->this_client->adapter->dev, "mpu_release\n");
	return result;
}

static noinline int mpudev_ioctl_rdrw(struct i2c_client *client,
				      unsigned long arg)
{
	struct i2c_rdwr_ioctl_data rdwr_arg;
	struct i2c_msg *rdwr_pa;
	u8 __user **data_ptrs;
	int i, res;

	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);

	if (copy_from_user(&rdwr_arg,
			   (struct i2c_rdwr_ioctl_data __user *)arg,
			   sizeof(rdwr_arg)))
		return -EFAULT;

	/* Put an arbitrary limit on the number of messages that can
	 * be sent at once */
	if (rdwr_arg.nmsgs > I2C_RDRW_IOCTL_MAX_MSGS)
		return -EINVAL;

	rdwr_pa = (struct i2c_msg *)
	    kmalloc(rdwr_arg.nmsgs * sizeof(struct i2c_msg), GFP_KERNEL);
	if (!rdwr_pa)
		return -ENOMEM;

	if (copy_from_user(rdwr_pa, rdwr_arg.msgs,
			   rdwr_arg.nmsgs * sizeof(struct i2c_msg))) {
		kfree(rdwr_pa);
		return -EFAULT;
	}

	data_ptrs = kmalloc(rdwr_arg.nmsgs * sizeof(u8 __user *), GFP_KERNEL);
	if (data_ptrs == NULL) {
		kfree(rdwr_pa);
		return -ENOMEM;
	}

	res = 0;
	for (i = 0; i < rdwr_arg.nmsgs; i++) {
		/* Limit the size of the message to a sane amount;
		 * and don't let length change either. */
		if ((rdwr_pa[i].len > 8192) ||
		    (rdwr_pa[i].flags & I2C_M_RECV_LEN)) {
			res = -EINVAL;
			break;
		}
		data_ptrs[i] = (u8 __user *) rdwr_pa[i].buf;
		rdwr_pa[i].buf = kmalloc(rdwr_pa[i].len, GFP_KERNEL);
		if (rdwr_pa[i].buf == NULL) {
			res = -ENOMEM;
			break;
		}
		if (copy_from_user(rdwr_pa[i].buf, data_ptrs[i],
				   rdwr_pa[i].len)) {
			++i;	/* Needs to be kfreed too */
			res = -EFAULT;
			break;
		}
	}
	if (res < 0) {
		int j;
		for (j = 0; j < i; ++j)
			kfree(rdwr_pa[j].buf);
		kfree(data_ptrs);
		kfree(rdwr_pa);
		return res;
	}

	res = i2c_transfer(client->adapter, rdwr_pa, rdwr_arg.nmsgs);
	while (i-- > 0) {
		if (res >= 0 && (rdwr_pa[i].flags & I2C_M_RD)) {
			if (copy_to_user(data_ptrs[i], rdwr_pa[i].buf,
					 rdwr_pa[i].len))
				res = -EFAULT;
		}
		kfree(rdwr_pa[i].buf);
	}
	kfree(data_ptrs);
	kfree(rdwr_pa);
	return res;
}

/* read function called when from /dev/mpu is read.  Read from the FIFO */
static ssize_t mpu_read(struct file *file,
			char __user *buf, size_t count, loff_t *offset)
{
	char *tmp;
	int ret;

	struct i2c_client *client = (struct i2c_client *)file->private_data;

	if (count > 8192)
		count = 8192;

	tmp = kmalloc(count, GFP_KERNEL);
	if (tmp == NULL)
		return -ENOMEM;

	pr_debug("i2c-dev: i2c-%d reading %zu bytes.\n",
		 iminor(file->f_path.dentry->d_inode), count);

/* @todo fix this to do a i2c trasnfer from the FIFO */
	ret = i2c_master_recv(client, tmp, count);
	if (ret >= 0) {
		ret = copy_to_user(buf, tmp, count) ? -EFAULT : ret;
		if (ret)
			ret = -EFAULT;
	}
	kfree(tmp);
	return ret;
}

static int mpu_ioctl_set_mpu_pdata(struct i2c_client *client, unsigned long arg)
{
	int ii;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mpu3050_platform_data *pdata = mpu->mldl_cfg.pdata;
	struct mpu3050_platform_data local_pdata;

	if (copy_from_user(&local_pdata, (unsigned char __user *)arg,
			   sizeof(local_pdata)))
		return -EFAULT;

	pdata->int_config = local_pdata.int_config;
	for (ii = 0; ii < DIM(pdata->orientation); ii++)
		pdata->orientation[ii] = local_pdata.orientation[ii];
	pdata->level_shifter = local_pdata.level_shifter;

	pdata->accel.address = local_pdata.accel.address;
	for (ii = 0; ii < DIM(pdata->accel.orientation); ii++)
		pdata->accel.orientation[ii] =
		    local_pdata.accel.orientation[ii];

	pdata->compass.address = local_pdata.compass.address;
	for (ii = 0; ii < DIM(pdata->compass.orientation); ii++)
		pdata->compass.orientation[ii] =
		    local_pdata.compass.orientation[ii];

	pdata->pressure.address = local_pdata.pressure.address;
	for (ii = 0; ii < DIM(pdata->pressure.orientation); ii++)
		pdata->pressure.orientation[ii] =
		    local_pdata.pressure.orientation[ii];

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

	return ML_SUCCESS;
}

static int
mpu_ioctl_set_mpu_config(struct i2c_client *client, unsigned long arg)
{
	int ii;
	int result = ML_SUCCESS;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mldl_cfg *temp_mldl_cfg;

	dev_dbg(&mpu->this_client->adapter->dev, "%s\n", __func__);

	temp_mldl_cfg = kzalloc(sizeof(struct mldl_cfg), GFP_KERNEL);
	if (NULL == temp_mldl_cfg)
		return -ENOMEM;

	/*
	 * User space is not allowed to modify accel compass pressure or
	 * pdata structs, as well as silicon_revision product_id or trim
	 */
	if (copy_from_user(temp_mldl_cfg, (struct mldl_cfg __user *)arg,
			   offsetof(struct mldl_cfg, silicon_revision))) {
		result = -EFAULT;
		goto out;
	}

	if (mldl_cfg->gyro_is_suspended) {
		if (mldl_cfg->addr != temp_mldl_cfg->addr)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->int_config != temp_mldl_cfg->int_config)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->ext_sync != temp_mldl_cfg->ext_sync)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->full_scale != temp_mldl_cfg->full_scale)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->lpf != temp_mldl_cfg->lpf)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->clk_src != temp_mldl_cfg->clk_src)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->divider != temp_mldl_cfg->divider)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_enable != temp_mldl_cfg->dmp_enable)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->fifo_enable != temp_mldl_cfg->fifo_enable)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_cfg1 != temp_mldl_cfg->dmp_cfg1)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->dmp_cfg2 != temp_mldl_cfg->dmp_cfg2)
			mldl_cfg->gyro_needs_reset = TRUE;

		if (mldl_cfg->gyro_power != temp_mldl_cfg->gyro_power)
			mldl_cfg->gyro_needs_reset = TRUE;

		for (ii = 0; ii < MPU_NUM_AXES; ii++)
			if (mldl_cfg->offset_tc[ii] !=
			    temp_mldl_cfg->offset_tc[ii])
				mldl_cfg->gyro_needs_reset = TRUE;

		for (ii = 0; ii < MPU_NUM_AXES; ii++)
			if (mldl_cfg->offset[ii] != temp_mldl_cfg->offset[ii])
				mldl_cfg->gyro_needs_reset = TRUE;

		if (memcmp(mldl_cfg->ram, temp_mldl_cfg->ram,
			   MPU_MEM_NUM_RAM_BANKS * MPU_MEM_BANK_SIZE *
			   sizeof(unsigned char)))
			mldl_cfg->gyro_needs_reset = TRUE;
	}

	memcpy(mldl_cfg, temp_mldl_cfg,
	       offsetof(struct mldl_cfg, silicon_revision));

out:
	kfree(temp_mldl_cfg);
	return result;
}

static int
mpu_ioctl_get_mpu_config(struct i2c_client *client, unsigned long arg)
{
	/* Have to be careful as there are 3 pointers in the mldl_cfg
	 * structure */
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mldl_cfg *local_mldl_cfg;
	int retval = 0;

	local_mldl_cfg = kzalloc(sizeof(struct mldl_cfg), GFP_KERNEL);
	if (NULL == local_mldl_cfg)
		return -ENOMEM;

	retval =
	    copy_from_user(local_mldl_cfg, (struct mldl_cfg __user *)arg,
			   sizeof(struct mldl_cfg));
	if (retval) {
		dev_err(&mpu->this_client->adapter->dev,
			"%s|%s:%d: EFAULT on arg\n",
			__FILE__, __func__, __LINE__);
		retval = -EFAULT;
		goto out;
	}

	/* Fill in the accel, compass, pressure and pdata pointers */
	if (mldl_cfg->accel) {
		retval = copy_to_user((void __user *)local_mldl_cfg->accel,
				      mldl_cfg->accel,
				      sizeof(*mldl_cfg->accel));
		if (retval) {
			dev_err(&mpu->this_client->adapter->dev,
				"%s|%s:%d: EFAULT on accel\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->compass) {
		retval = copy_to_user((void __user *)local_mldl_cfg->compass,
				      mldl_cfg->compass,
				      sizeof(*mldl_cfg->compass));
		if (retval) {
			dev_err(&mpu->this_client->adapter->dev,
				"%s|%s:%d: EFAULT on compass\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->pressure) {
		retval = copy_to_user((void __user *)local_mldl_cfg->pressure,
				      mldl_cfg->pressure,
				      sizeof(*mldl_cfg->pressure));
		if (retval) {
			dev_err(&mpu->this_client->adapter->dev,
				"%s|%s:%d: EFAULT on pressure\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	if (mldl_cfg->pdata) {
		retval = copy_to_user((void __user *)local_mldl_cfg->pdata,
				      mldl_cfg->pdata,
				      sizeof(*mldl_cfg->pdata));
		if (retval) {
			dev_err(&mpu->this_client->adapter->dev,
				"%s|%s:%d: EFAULT on pdata\n",
				__FILE__, __func__, __LINE__);
			retval = -EFAULT;
			goto out;
		}
	}

	/* Do not modify the accel, compass, pressure and pdata pointers */
	retval = copy_to_user((struct mldl_cfg __user *)arg,
			      mldl_cfg, offsetof(struct mldl_cfg, accel));

	if (retval)
		retval = -EFAULT;
out:
	kfree(local_mldl_cfg);
	return retval;
}

/**
 * Pass a requested slave configuration to the slave sensor
 *
 * @param adapter the adaptor to use to communicate with the slave
 * @param mldl_cfg the mldl configuration structuer
 * @param slave pointer to the slave descriptor
 * @param usr_config The configuration to pass to the slave sensor
 *
 * @return 0 or non-zero error code
 */
static int slave_config(void *adapter,
			struct mldl_cfg *mldl_cfg,
			struct ext_slave_descr *slave,
			struct ext_slave_config __user *usr_config)
{
	int retval = ML_SUCCESS;
	if ((slave) && (slave->config)) {
		struct ext_slave_config config;
		retval = copy_from_user(&config, usr_config, sizeof(config));
		if (retval)
			return -EFAULT;

		if (config.len && config.data) {
			int *data;
			data = kzalloc(config.len, GFP_KERNEL);
			if (!data)
				return ML_ERROR_MEMORY_EXAUSTED;

			retval = copy_from_user(data,
						(void __user *)config.data,
						config.len);
			if (retval) {
				retval = -EFAULT;
				kfree(data);
				return retval;
			}
			config.data = data;
		}
		retval = slave->config(adapter,
				       slave, &mldl_cfg->pdata->accel, &config);
		kfree(config.data);
	}
	return retval;
}

/**
 * Get a requested slave configuration from the slave sensor
 *
 * @param adapter the adaptor to use to communicate with the slave
 * @param mldl_cfg the mldl configuration structuer
 * @param slave pointer to the slave descriptor
 * @param usr_config The configuration for the slave to fill out
 *
 * @return 0 or non-zero error code
 */
static int slave_get_config(void *adapter,
			    struct mldl_cfg *mldl_cfg,
			    struct ext_slave_descr *slave,
			    struct ext_slave_config __user *usr_config)
{
	int retval = ML_SUCCESS;
	if ((slave) && (slave->get_config)) {
		struct ext_slave_config config;
		void *user_data;
		retval = copy_from_user(&config, usr_config, sizeof(config));
		if (retval)
			return -EFAULT;

		user_data = config.data;
		if (config.len && config.data) {
			int *data;
			data = kzalloc(config.len, GFP_KERNEL);
			if (!data)
				return ML_ERROR_MEMORY_EXAUSTED;

			retval = copy_from_user(data,
						(void __user *)config.data,
						config.len);
			if (retval) {
				retval = -EFAULT;
				kfree(data);
				return retval;
			}
			config.data = data;
		}
		retval = slave->get_config(adapter,
					   slave,
					   &mldl_cfg->pdata->accel, &config);
		if (retval) {
			kfree(config.data);
			return retval;
		}
		retval = copy_to_user((unsigned char __user *)user_data,
				      config.data, config.len);
		kfree(config.data);
	}
	return retval;
}

/* ioctl - I/O control */
static long mpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = (struct i2c_client *)file->private_data;
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	int retval = 0;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	switch (cmd) {
	case I2C_RDWR:
		mpudev_ioctl_rdrw(client, arg);
		break;
	case I2C_SLAVE:
		if ((arg & 0x7E) != (client->addr & 0x7E)) {
			dev_err(&mpu->this_client->adapter->dev,
				"%s: Invalid I2C_SLAVE arg %lu\n",
				__func__, arg);
		}
		break;
	case MPU_SET_MPU_CONFIG:
		retval = mpu_ioctl_set_mpu_config(client, arg);
		break;
	case MPU_SET_INT_CONFIG:
		mldl_cfg->int_config = (unsigned char)arg;
		break;
	case MPU_SET_EXT_SYNC:
		mldl_cfg->ext_sync = (enum mpu_ext_sync)arg;
		break;
	case MPU_SET_FULL_SCALE:
		mldl_cfg->full_scale = (enum mpu_fullscale)arg;
		break;
	case MPU_SET_LPF:
		mldl_cfg->lpf = (enum mpu_filter)arg;
		break;
	case MPU_SET_CLK_SRC:
		mldl_cfg->clk_src = (enum mpu_clock_sel)arg;
		break;
	case MPU_SET_DIVIDER:
		mldl_cfg->divider = (unsigned char)arg;
		break;
	case MPU_SET_LEVEL_SHIFTER:
		mldl_cfg->pdata->level_shifter = (unsigned char)arg;
		break;
	case MPU_SET_DMP_ENABLE:
		mldl_cfg->dmp_enable = (unsigned char)arg;
		break;
	case MPU_SET_FIFO_ENABLE:
		mldl_cfg->fifo_enable = (unsigned char)arg;
		break;
	case MPU_SET_DMP_CFG1:
		mldl_cfg->dmp_cfg1 = (unsigned char)arg;
		break;
	case MPU_SET_DMP_CFG2:
		mldl_cfg->dmp_cfg2 = (unsigned char)arg;
		break;
	case MPU_SET_OFFSET_TC:
		retval = copy_from_user(mldl_cfg->offset_tc,
					(unsigned char __user *)arg,
					sizeof(mldl_cfg->offset_tc));
		if (retval)
			retval = -EFAULT;

		break;
	case MPU_SET_RAM:
		retval = copy_from_user(mldl_cfg->ram,
					(unsigned char __user *)arg,
					sizeof(mldl_cfg->ram));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_SET_PLATFORM_DATA:
		retval = mpu_ioctl_set_mpu_pdata(client, arg);
		break;
	case MPU_GET_MPU_CONFIG:
		retval = mpu_ioctl_get_mpu_config(client, arg);
		break;
	case MPU_GET_INT_CONFIG:
		retval = put_user(mldl_cfg->int_config,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_EXT_SYNC:
		retval = put_user(mldl_cfg->ext_sync,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_FULL_SCALE:
		retval = put_user(mldl_cfg->full_scale,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_LPF:
		retval = put_user(mldl_cfg->lpf, (unsigned char __user *)arg);
		break;
	case MPU_GET_CLK_SRC:
		retval = put_user(mldl_cfg->clk_src,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_DIVIDER:
		retval = put_user(mldl_cfg->divider,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_LEVEL_SHIFTER:
		retval = put_user(mldl_cfg->pdata->level_shifter,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_DMP_ENABLE:
		retval = put_user(mldl_cfg->dmp_enable,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_FIFO_ENABLE:
		retval = put_user(mldl_cfg->fifo_enable,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_DMP_CFG1:
		retval = put_user(mldl_cfg->dmp_cfg1,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_DMP_CFG2:
		retval = put_user(mldl_cfg->dmp_cfg2,
				  (unsigned char __user *)arg);
		break;
	case MPU_GET_OFFSET_TC:
		retval = copy_to_user((unsigned char __user *)arg,
				      mldl_cfg->offset_tc,
				      sizeof(mldl_cfg->offset_tc));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_GET_RAM:
		retval = copy_to_user((unsigned char __user *)arg,
				      mldl_cfg->ram, sizeof(mldl_cfg->ram));
		if (retval)
			retval = -EFAULT;
		break;
	case MPU_CONFIG_ACCEL:
		retval = slave_config(accel_adapter, mldl_cfg,
				      mldl_cfg->accel,
				      (struct ext_slave_config __user *)arg);
		break;
	case MPU_CONFIG_COMPASS:
		retval = slave_config(compass_adapter, mldl_cfg,
				      mldl_cfg->compass,
				      (struct ext_slave_config __user *)arg);
		break;
	case MPU_CONFIG_PRESSURE:
		retval = slave_config(pressure_adapter, mldl_cfg,
				      mldl_cfg->pressure,
				      (struct ext_slave_config __user *)arg);
		break;
	case MPU_GET_CONFIG_ACCEL:
		retval = slave_get_config(accel_adapter, mldl_cfg,
					  mldl_cfg->accel,
					  (struct ext_slave_config __user *)
					  arg);
		break;
	case MPU_GET_CONFIG_COMPASS:
		retval = slave_get_config(compass_adapter, mldl_cfg,
					  mldl_cfg->compass,
					  (struct ext_slave_config __user *)
					  arg);
		break;
	case MPU_GET_CONFIG_PRESSURE:
		retval = slave_get_config(pressure_adapter, mldl_cfg,
					  mldl_cfg->pressure,
					  (struct ext_slave_config __user *)
					  arg);
		break;
	case MPU_SUSPEND:
		{
			unsigned long sensors;
			sensors = ~(mldl_cfg->requested_sensors);
			retval = mpu3050_suspend(mldl_cfg,
						 client->adapter,
						 accel_adapter,
						 compass_adapter,
						 pressure_adapter,
						 ((sensors & ML_THREE_AXIS_GYRO)
						  == ML_THREE_AXIS_GYRO),
						 ((sensors &
						   ML_THREE_AXIS_ACCEL)
						  == ML_THREE_AXIS_ACCEL),
						 ((sensors &
						   ML_THREE_AXIS_COMPASS)
						  == ML_THREE_AXIS_COMPASS),
						 ((sensors &
						   ML_THREE_AXIS_PRESSURE)
						  == ML_THREE_AXIS_PRESSURE));
		}
		break;
	case MPU_RESUME:
		{
			unsigned long sensors;
			sensors = mldl_cfg->requested_sensors;
			retval = mpu3050_resume(mldl_cfg,
						client->adapter,
						accel_adapter,
						compass_adapter,
						pressure_adapter,
						sensors & ML_THREE_AXIS_GYRO,
						sensors & ML_THREE_AXIS_ACCEL,
						sensors & ML_THREE_AXIS_COMPASS,
						sensors &
						ML_THREE_AXIS_PRESSURE);
		}
		break;
	case MPU_READ_ACCEL:
		{
			unsigned char data[6];
			int x;
			int y;
			int z;

			retval = mpu3050_read_accel(mldl_cfg, client->adapter,
						    data);

			x = (s16)((data[1] << 4) | (data[0] >> 4))
				+ mpu->cal_data.x;
			y = (s16)((data[3] << 4) | (data[2] >> 4))
				+ mpu->cal_data.y;
			z = (s16)((data[5] << 4) | (data[4] >> 4))
				+ mpu->cal_data.z;

			data[0] = (x & 0xf) << 4;
			data[1] = (x & 0xff0) >> 4;
			data[2] = (y & 0xf) << 4;
			data[3] = (y & 0xff0) >> 4;
			data[4] = (z & 0xf) << 4;
			data[5] = (z & 0xff0) >> 4;

			if ((ML_SUCCESS == retval) &&
			    (copy_to_user((unsigned char __user *)arg,
					  data, sizeof(data))))
				retval = -EFAULT;
		}
		break;
	case MPU_READ_COMPASS:
		{
			unsigned char data[7];
			struct i2c_adapter *compass_adapt =
			    i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
			retval = mpu3050_read_compass(mldl_cfg, compass_adapt,
						      data);
			if ((ML_SUCCESS == retval) &&
			    (copy_to_user((unsigned char *)arg,
					  data, sizeof(data))))
				retval = -EFAULT;
		}
		break;
	case MPU_READ_PRESSURE:
		{
			unsigned char data[3];
			struct i2c_adapter *pressure_adapt =
			    i2c_get_adapter(mldl_cfg->pdata->pressure.
					    adapt_num);
			retval =
			    mpu3050_read_pressure(mldl_cfg, pressure_adapt,
						  data);
			if ((ML_SUCCESS == retval)
			    &&
			    (copy_to_user
			     ((unsigned char __user *)arg, data, sizeof(data))))
				retval = -EFAULT;
		}
		break;
	case MPU_READ_MEMORY:
	case MPU_WRITE_MEMORY:
	default:
		dev_err(&mpu->this_client->adapter->dev,
			"%s: Unknown cmd %d, arg %lu\n", __func__, cmd, arg);
		retval = -EINVAL;
	}

	return retval;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
void mpu3050_early_suspend(struct early_suspend *h)
{
	struct mpu_private_data *mpu = container_of(h,
						    struct
						    mpu_private_data,
						    early_suspend);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	pr_info("@@@@@ mpu3050_early_suspend @@@@@\n");

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	dev_dbg(&mpu->this_client->adapter->dev, "%s: %d, %d\n", __func__,
		h->level, mpu->mldl_cfg.gyro_is_suspended);
		(void)mpu3050_suspend(mldl_cfg, mpu->this_client->adapter,
				      accel_adapter, compass_adapter,
				      pressure_adapter, TRUE, TRUE, TRUE, TRUE);
}

void mpu3050_early_resume(struct early_suspend *h)
{
	struct mpu_private_data *mpu = container_of(h,
						    struct
						    mpu_private_data,
						    early_suspend);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

		if (mpu->pid) {
			unsigned long sensors = mldl_cfg->requested_sensors;
			(void)mpu3050_resume(mldl_cfg,
					     mpu->this_client->adapter,
					     accel_adapter,
					     compass_adapter,
					     pressure_adapter,
					     sensors & ML_THREE_AXIS_GYRO,
					     sensors & ML_THREE_AXIS_ACCEL,
					     sensors & ML_THREE_AXIS_COMPASS,
					     sensors & ML_THREE_AXIS_PRESSURE);
			dev_dbg(&mpu->this_client->adapter->dev,
				"%s for pid %d\n", __func__, mpu->pid);
		}
	dev_dbg(&mpu->this_client->adapter->dev,
		"%s: %d\n", __func__, h->level);
}
#endif

void mpu_shutdown(struct i2c_client *client)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	(void)mpu3050_suspend(mldl_cfg, mpu->this_client->adapter,
			      accel_adapter, compass_adapter, pressure_adapter,
			      TRUE, TRUE, TRUE, TRUE);
	dev_dbg(&mpu->this_client->adapter->dev, "%s\n", __func__);
}

int mpu_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	if (!mpu->mldl_cfg.gyro_is_suspended) {
		dev_dbg(&mpu->this_client->adapter->dev,
			"%s: suspending on event %d\n", __func__, mesg.event);
		(void)mpu3050_suspend(mldl_cfg, mpu->this_client->adapter,
				      accel_adapter, compass_adapter,
				      pressure_adapter, TRUE, TRUE, TRUE, TRUE);
	} else {
		dev_dbg(&mpu->this_client->adapter->dev,
			"%s: Already suspended %d\n", __func__, mesg.event);
	}
	return 0;
}

int mpu_resume(struct i2c_client *client)
{
	struct mpu_private_data *mpu =
	    (struct mpu_private_data *)i2c_get_clientdata(client);
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	if (mpu->pid) {
		unsigned long sensors = mldl_cfg->requested_sensors;
		(void)mpu3050_resume(mldl_cfg, mpu->this_client->adapter,
				     accel_adapter,
				     compass_adapter,
				     pressure_adapter,
				     sensors & ML_THREE_AXIS_GYRO,
				     sensors & ML_THREE_AXIS_ACCEL,
				     sensors & ML_THREE_AXIS_COMPASS,
				     sensors & ML_THREE_AXIS_PRESSURE);
		dev_dbg(&mpu->this_client->adapter->dev,
			"%s for pid %d\n", __func__, mpu->pid);
	}
	return 0;
}

/* define which file operations are supported */
static const struct file_operations mpu_fops = {
	.owner = THIS_MODULE,
	.read = mpu_read,
#if HAVE_COMPAT_IOCTL
	.compat_ioctl = mpu_ioctl,
#endif
#if HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = mpu_ioctl,
#endif
	.open = mpu_open,
	.release = mpu_release,
};

static unsigned short normal_i2c[] = { I2C_CLIENT_END };

static struct miscdevice i2c_mpu_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mpu",		/* Same for both 3050 and 6000 */
	.fops = &mpu_fops,
};

#define FACTORY_TEST
#ifdef FACTORY_TEST
static ssize_t mpu3050_power_on(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int count = 0;

	dev_dbg(dev, "this_client = %d\n", (int)mpu_private_data->this_client);
	count = sprintf(buf, "%d\n",
		(mpu_private_data->this_client != NULL ? 1 : 0));

	return count;
}

static int mpu3050_factory_on(struct i2c_client *client)
{
	struct mldl_cfg *mldl_cfg = &mpu_private_data->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;
	int prev_gyro_suspended;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	prev_gyro_suspended = mldl_cfg->gyro_is_suspended;


	unsigned long sensors = mldl_cfg->requested_sensors;
	(void)mpu3050_resume(mldl_cfg, mpu_private_data->this_client->adapter,
			     accel_adapter,
			     compass_adapter,
			     pressure_adapter,
			     sensors & ML_THREE_AXIS_GYRO,
			     sensors & ML_THREE_AXIS_ACCEL,
			     sensors & ML_THREE_AXIS_COMPASS,
			     sensors & ML_THREE_AXIS_PRESSURE);

	pr_info("%s prev_gyro_suspended ", __func__);

	return prev_gyro_suspended;
}

static void mpu3050_factory_off(struct i2c_client *client)
{
	struct mldl_cfg *mldl_cfg = &mpu_private_data->mldl_cfg;
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);

	pr_info("%s: suspending on event\n", __func__);
	(void)mpu3050_suspend(mldl_cfg, mpu_private_data->this_client->adapter,
			      accel_adapter, compass_adapter,
			      pressure_adapter, TRUE, TRUE, TRUE, TRUE);
	return;
}

static ssize_t mpu3050_get_temp(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int count = 0;
	short int temperature = 0;
	unsigned char data[2];
	int prev_gyro_suspended;

	prev_gyro_suspended = mpu3050_factory_on(mpu_private_data->this_client);

	sensor_i2c_read(mpu_private_data->this_client->adapter,
		DEFAULT_MPU_SLAVEADDR, MPUREG_TEMP_OUT_H, 2, data);

	temperature = (short) (((data[0]) << 8) | data[1]);
	temperature = (((temperature + 521) / 340) + 35);

	pr_info("read temperature = %d\n", temperature);

	count = sprintf(buf, "%d\n", temperature);

	return count;
}

/*
    Defines
*/

#define DEBUG_OUT 1

#define DEF_GYRO_FULLSCALE       (2000)	/* gyro full scale dps        */
#define DEF_GYRO_SENS            (32768.f/DEF_GYRO_FULLSCALE)
					       /* gyro sensitivity LSB/dps   */
#define DEF_PACKET_THRESH        (75)	/* 600 ms / 8ms / sample      */
#define DEF_TIMING_TOL           (.05f)	/* 5%                         */
#define DEF_BIAS_THRESH          (40*DEF_GYRO_SENS)
					       /* 40 dps in LSBs             */
#define DEF_RMS_THRESH_SQ        (0.4f*0.4f*DEF_GYRO_SENS*DEF_GYRO_SENS)
					       /* (.2 dps in LSBs ) ^ 2      */
#define DEF_TEST_TIME_PER_AXIS   (600)	/* ms of time spent collecting
					   data for each axis,
					   multiple of 600ms          */

/*
    Macros
*/

#define CHECK_TEST_ERROR(x)    \
	if (x) {                                \
		pr_info("error %d @ %s|%d\n",      \
		x, __func__, __LINE__);                \
		return -1;                             \
	}

#define SHORT_TO_TEMP_C(shrt)         (((shrt+13200)/280)+35)
#define CHARS_TO_SHORT(d)             ((((short)(d)[0]) << 8)+(d)[1])
#define fabs(x)      (((x) < 0) ? -(x) : (x))

#define X   (0)
#define Y   (1)
#define Z   (2)

static short mpu3050_selftest_gyro_avg[3];
static int mpu3050_selftest_result;
static int mpu3050_selftest_bias[3];
static int mpu3050_selftest_rms[3];

int mpu3050_test_gyro(struct i2c_client *client, short gyro_biases[3],
		      short *temp_avg)
{
	void *mlsl_handle = client->adapter;
	int retVal = 0;
	unsigned char result;

	int total_count = 0;
	int total_count_axis[3] = { 0, 0, 0 };
	int packet_count;
	unsigned char regs[7];

	char a_name[3][2] = { "X", "Y", "Z" };
	int temperature;
	int Avg[3];
	int RMS[3];
	int i, j, tmp;
	unsigned char dataout[20];

	short *x, *y, *z;

	x = kzalloc(sizeof(*x) * (DEF_TEST_TIME_PER_AXIS / 8 * 4), GFP_KERNEL);
	y = kzalloc(sizeof(*y) * (DEF_TEST_TIME_PER_AXIS / 8 * 4), GFP_KERNEL);
	z = kzalloc(sizeof(*z) * (DEF_TEST_TIME_PER_AXIS / 8 * 4), GFP_KERNEL);

	temperature = 0;

	/* sample rate = 8ms */
	result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
				       MPUREG_SMPLRT_DIV, 0x07);
	CHECK_TEST_ERROR(result);

	regs[0] = 0x03;		/* filter = 42Hz, analog_sample rate = 1 KHz */
	switch (DEF_GYRO_FULLSCALE) {
	case 2000:
		regs[0] |= 0x18;
		break;
	case 1000:
		regs[0] |= 0x10;
		break;
	case 500:
		regs[0] |= 0x08;
		break;
	case 250:
	default:
		regs[0] |= 0x00;
		break;
	}
	result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
				       MPUREG_DLPF_FS_SYNC, regs[0]);
	CHECK_TEST_ERROR(result);
	result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
				       MPUREG_INT_CFG, 0x00);

	/* 1st, timing test */
	for (j = 0; j < 3; j++) {

		pr_info("Collecting gyro data from %s gyro PLL\n", a_name[j]);

		/* turn on all gyros, use gyro X for clocking
		   Set to Y and Z for 2nd and 3rd iteration */
		result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
					       MPUREG_PWR_MGM, j + 1);
		CHECK_TEST_ERROR(result);

		/* wait for 2 ms after switching clock source */
		mdelay(2);

		/* we will enable XYZ gyro in FIFO and nothing else */
		result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
					       MPUREG_FIFO_EN2, 0x00);
		CHECK_TEST_ERROR(result);
		/* enable/reset FIFO */
		result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
					       MPUREG_USER_CTRL, 0x42);

		tmp = (int)(DEF_TEST_TIME_PER_AXIS / 600);
		while (tmp-- > 0) {
			/* enable XYZ gyro in FIFO and nothing else */
			result =
			    MLSLSerialWriteSingle(mlsl_handle, client->addr,
						  MPUREG_FIFO_EN1, 0x70);
			CHECK_TEST_ERROR(result);

			/* wait for 600 ms for data */
			msleep(600);

			/* stop storing gyro in the FIFO */
			result =
			    MLSLSerialWriteSingle(mlsl_handle, client->addr,
						  MPUREG_FIFO_EN1, 0x00);
			CHECK_TEST_ERROR(result);

			/* Getting number of bytes in FIFO */
			result = MLSLSerialRead(mlsl_handle, client->addr,
						MPUREG_FIFO_COUNTH, 2, dataout);
			CHECK_TEST_ERROR(result);
			/* number of 6 B packets in the FIFO */
			packet_count = CHARS_TO_SHORT(dataout) / 6;
			pr_info("Packet Count: %d - ", packet_count);

			if (abs(packet_count - DEF_PACKET_THRESH)
			    <=	/* Within +-5% range */
			    (int)(DEF_TIMING_TOL * DEF_PACKET_THRESH + .5)) {
				for (i = 0; i < packet_count; i++) {
					/* getting FIFO data */
					result =
					    MLSLSerialReadFifo(mlsl_handle,
							       client->addr, 6,
							       dataout);
					CHECK_TEST_ERROR(result);
					x[total_count + i] =
					    CHARS_TO_SHORT(&dataout[0]);
					y[total_count + i] =
					    CHARS_TO_SHORT(&dataout[2]);
					z[total_count + i] =
					    CHARS_TO_SHORT(&dataout[4]);
				}
				total_count += packet_count;
				total_count_axis[j] += packet_count;
				pr_info("OK\n");
			} else {
				retVal |= 1 << j;
				pr_info("NOK - samples ignored\n");
			}
		}

		/* remove gyros from FIFO */
		result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
					       MPUREG_FIFO_EN1, 0x00);
		CHECK_TEST_ERROR(result);

		/* Read Temperature */
		result = MLSLSerialRead(mlsl_handle, client->addr,
					MPUREG_TEMP_OUT_H, 2, dataout);
		temperature += (short)CHARS_TO_SHORT(dataout);
	}

	pr_info("\n Total %d samples\n\n", total_count);

	/* 2nd, check bias from X and Y PLL clock source */
	tmp = total_count != 0 ? total_count : 1;
	for (i = 0,
	     Avg[X] = .0f, Avg[Y] = .0f, Avg[Z] = .0f; i < total_count; i++) {
		Avg[X] += x[i];
		Avg[Y] += y[i];
		Avg[Z] += z[i];
	}

	Avg[X] /= tmp;
	Avg[Y] /= tmp;
	Avg[Z] /= tmp;

	pr_info("\n bias          : %+13d %+13d %+13d (LSB)\n",
	       Avg[X], Avg[Y], Avg[Z]);
	if (DEBUG_OUT) {
		pr_info("              : %+13d %+13d %+13d (dps)\n",
		       Avg[X] / 131, Avg[Y] / 131, Avg[Z] / 131);
	}

	/* 3rd and finally, check RMS */
	for (i = 0,
	     RMS[X] = 0.f, RMS[Y] = 0.f, RMS[Z] = 0.f; i < total_count; i++) {
		RMS[X] += (x[i] - Avg[X]) * (x[i] - Avg[X]);
		RMS[Y] += (y[i] - Avg[Y]) * (y[i] - Avg[Y]);
		RMS[Z] += (z[i] - Avg[Z]) * (z[i] - Avg[Z]);
	}
	for (j = 0; j < 3; j++) {
		if (RMS[j] > (int)DEF_RMS_THRESH_SQ * total_count) {
			pr_info
			    ("\n %s-Gyro RMS (%d) exceeded threshold (%.4f)\n",
			     a_name[j], RMS[j] / total_count,
			     DEF_RMS_THRESH_SQ);
			retVal |= 1 << (6 + j);
		}

	}
	pr_info("\n RMS^2           : %+13d %+13d %+13d (LSB-rms)\n",
	       (RMS[X] / total_count),
	       (RMS[Y] / total_count), (RMS[Z] / total_count));
	if (RMS[X] == 0 || RMS[Y] == 0 || RMS[Z] == 0) {
		/*If any of the RMS noise value returns zero,
		   then we might have dead gyro or FIFO/register failure,
		   or the part is sleeping */
		retVal |= 1 << 9;
	}

	temperature /= 3;
	if (DEBUG_OUT)
		pr_info("\n Temperature   : %+13d %13s %13s (deg. C)\n",
		       SHORT_TO_TEMP_C(temperature), "", "");

	/* load into final storage */
	*temp_avg = (short)temperature;
	gyro_biases[X] = (short)Avg[X];
	gyro_biases[Y] = (short)Avg[Y];
	gyro_biases[Z] = (short)Avg[Z];

	mpu3050_selftest_bias[X] = (int)Avg[X];
	mpu3050_selftest_bias[Y] = (int)Avg[Y];
	mpu3050_selftest_bias[Z] = (int)Avg[Z];

	mpu3050_selftest_rms[X] = RMS[X] / total_count;
	mpu3050_selftest_rms[Y] = RMS[Y] / total_count;
	mpu3050_selftest_rms[Z] = RMS[Z] / total_count;

	return retVal;
}

int mpu3050_self_test_once(struct i2c_client *client)
{
	void *mlsl_handle = client->adapter;
	int result = 0;

	short temp_avg;

	unsigned char regs[5];

	pr_info("\n Collecting %d groups of 600 ms samples for each axis\n\n",
	       DEF_TEST_TIME_PER_AXIS / 600);

	result = MLSLSerialRead(mlsl_handle, client->addr,
				MPUREG_PWR_MGM, 1, regs);
	CHECK_TEST_ERROR(result);
	/* reset */
	result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
				       MPUREG_PWR_MGM, regs[0] | 0x80);
	CHECK_TEST_ERROR(result);
	mdelay(5);
	/* wake up */
	if (regs[0] & 0x40) {
		result = MLSLSerialWriteSingle(mlsl_handle, client->addr,
					       MPUREG_PWR_MGM, 0x00);
		CHECK_TEST_ERROR(result);

	}
	msleep(60);

	/* collect gyro and temperature data */
	mpu3050_selftest_result = mpu3050_test_gyro(client,
						    mpu3050_selftest_gyro_avg,
						    &temp_avg);

	return mpu3050_selftest_result;
}

static ssize_t mpu3050_self_test(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned char gyro_data[6];
	short int raw[3];
	int count = 0;
	int res = 0;
	int prev_gyro_suspended;

	prev_gyro_suspended = mpu3050_factory_on(mpu_private_data->this_client);

	mpu3050_self_test_once(mpu_private_data->this_client);

	res = sensor_i2c_read(mpu_private_data->this_client->adapter,
		DEFAULT_MPU_SLAVEADDR, MPUREG_GYRO_XOUT_H, 6, gyro_data);

	if (res)
		return 0;
	raw[0] = (short)(((gyro_data[0]) << 8) | gyro_data[1]);
	raw[1] = (short)(((gyro_data[2]) << 8) | gyro_data[3]);
	raw[2] = (short)(((gyro_data[4]) << 8) | gyro_data[5]);

	pr_info("\n [mpu3050_self_test] %s: %s, %d, %d, %d, %d, %d, %d\n",
	       __func__,
	       (!mpu3050_selftest_result ? "OK" : "NG"),
	       raw[0], raw[1], raw[2],
	       mpu3050_selftest_gyro_avg[0],
	       mpu3050_selftest_gyro_avg[1], mpu3050_selftest_gyro_avg[2]);

	count = sprintf(buf, "%s, %d, %d, %d, %d, %d, %d\n",
			(!mpu3050_selftest_result ? "OK" : "NG"),
			mpu3050_selftest_bias[0], mpu3050_selftest_bias[1],
			mpu3050_selftest_bias[2], mpu3050_selftest_rms[0],
			mpu3050_selftest_rms[1], mpu3050_selftest_rms[2]);

	mpu3050_factory_off(mpu_private_data->this_client);
	mpu3050_factory_on(mpu_private_data->this_client);

	return count;
}

int Convert(int data)
{
	int temp;

	if (data < 2048)
		temp = data + 2048;
	else
		temp = 2048 - (4096 - data);
	return temp;
}

static ssize_t mpu3050_acc_read(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned char acc_data[6];
	unsigned char acc_ori[9];
	s16 x, y, z;
	int count;
	s32 temp;
	int i = 0;

	struct mldl_cfg *mldl_cfg = &mpu_private_data->mldl_cfg;

	if (mldl_cfg->accel_is_suspended == 1 ||
	    (mldl_cfg->dmp_is_running == 0
	     && mldl_cfg->accel_is_suspended == 0)) {
		sensor_i2c_read(mpu_private_data->this_client->adapter,
			0x0F, 0x06, 6, acc_data);
	} else if (mldl_cfg->dmp_is_running &&
		   mldl_cfg->accel_is_suspended == 0) {

		sensor_i2c_read(mpu_private_data->this_client->adapter,
			DEFAULT_MPU_SLAVEADDR, 0x23, 6, acc_data);
	}

	temp = (s16) ((acc_data[1] << 4) | (acc_data[0] >> 4))
		+ mpu_private_data->cal_data.x;
	if (temp < 2048)
		x = (s16) (temp);
	else
		x = (s16) ((4096 - temp)) * (-1);

	temp = (s16) ((acc_data[3] << 4) | (acc_data[2] >> 4))
		+ mpu_private_data->cal_data.y;
	if (temp < 2048)
		y = (s16) (temp);
	else
		y = (s16) ((4096 - temp)) * (-1);

	temp = (s16) ((acc_data[5] << 4) | (acc_data[4] >> 4))
		+ mpu_private_data->cal_data.z;
	if (temp < 2048)
		z = (s16) (temp);
	else
		z = (s16) ((4096 - temp)) * (-1);

	for (i = 0; i < 9; i++)
		acc_ori[i] = mldl_cfg->pdata->accel.orientation[i];

	if (acc_ori[0] == 0) {
		if (acc_ori[1] == 1) {
			if (acc_ori[3] == 1) {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", -y, -x,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", -y, -x,
						    z);
				}
			} else {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", y, -x,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", y, -x,
						    z);
				}
			}
		} else {
			if (acc_ori[3] == 1) {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", y, -x,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", -y, x,
						    z);
				}
			} else {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", y, x,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", y, x,
						    z);
				}
			}
		}
	} else {
		if (acc_ori[0] == 1) {
			if (acc_ori[4] == 1) {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", -x, -y,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", -x, -y,
						    z);
				}
			} else {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", -x, y,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", -x, y,
						    z);
				}
			}
		} else {
			if (acc_ori[4] == 1) {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", x, -y,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", x, -y,
						    z);
				}
			} else {
				if (acc_ori[8] == 1) {
					count =
					    sprintf(buf, "%d, %d, %d\n", x, y,
						    -z);
				} else {
					count =
					    sprintf(buf, "%d, %d, %d\n", x, y,
						    z);
				}
			}
		}
	}
	return count;
}

static ssize_t accel_calibration_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int count;
	pr_info(" accel_calibration_show %d %d %d\n",
		mpu_private_data->cal_data.x,
		mpu_private_data->cal_data.y,
		mpu_private_data->cal_data.z);
	count = sprintf(buf, "%d %d %d\n",
		mpu_private_data->cal_data.x,
		mpu_private_data->cal_data.y,
		mpu_private_data->cal_data.z);
	return count;

}
static ssize_t accel_calibration_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t size)
{
	int err;
	int count;
	unsigned long enable = 0;
	if (strict_strtoul(buf, 10, &enable))
		return -EINVAL;

	err = accel_do_calibrate(mpu_private_data, enable);
	if (err < 0)
		pr_err("%s: accel_do_calibrate() failed\n", __func__);

	pr_info("%d %d %d\n",
		mpu_private_data->cal_data.x,
		mpu_private_data->cal_data.y,
		mpu_private_data->cal_data.z);
	if (err > 0)
		err = 0;
	count = sprintf(buf, "%d\n", err);
	return count;
}

static DEVICE_ATTR(calibration, 0664, accel_calibration_show,
	accel_calibration_store);

static DEVICE_ATTR(power_on, S_IRUGO, mpu3050_power_on, NULL);
static DEVICE_ATTR(temperature, S_IRUGO, mpu3050_get_temp, NULL);
static DEVICE_ATTR(selftest, S_IRUGO, mpu3050_self_test, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, mpu3050_acc_read, NULL);

static struct device_attribute *gyro_sensor_attrs[] = {
	&dev_attr_power_on,
	&dev_attr_temperature,
	&dev_attr_selftest,
	NULL,
};

static struct device_attribute *accel_sensor_attrs[] = {
	&dev_attr_raw_data,
	&dev_attr_calibration,
	NULL,
};

static struct device *gyro_sensor_device;
static struct device *accel_sensor_device;
#endif

int mpu3050_probe(struct i2c_client *client, const struct i2c_device_id *devid)
{
	struct mpu3050_platform_data *pdata;
	struct mpu_private_data *mpu;
	struct mldl_cfg *mldl_cfg;
	int res = 0;
	struct i2c_adapter *accel_adapter = NULL;
	struct i2c_adapter *compass_adapter = NULL;
	struct i2c_adapter *pressure_adapter = NULL;

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		res = -ENODEV;
		goto out_check_functionality_failed;
	}

	mpu = kzalloc(sizeof(struct mpu_private_data), GFP_KERNEL);
	if (!mpu) {
		res = -ENOMEM;
		goto out_alloc_data_failed;
	}

	mutex_init(&mpu->power_lock);
	mpu->pid = 0;
	mpu->this_client = client;
	mpu_private_data = mpu;
	i2c_set_clientdata(client, mpu);


#ifdef FACTORY_TEST
	res =
	    sensors_register(gyro_sensor_device, NULL, gyro_sensor_attrs,
			     "gyro_sensor");
	if (res) {
		pr_info("%s: cound not register gyro sensor device(%d).\n",
		       __func__, res);
	}

	res =
	    sensors_register(accel_sensor_device, NULL, accel_sensor_attrs,
			     "accelerometer_sensor");
	if (res) {
		pr_info("%s: cound not register accelerometer sensor device(%d).\n",
		       __func__, res);
	}
#endif

	mldl_cfg = &mpu->mldl_cfg;
	pdata = (struct mpu3050_platform_data *)client->dev.platform_data;
	if (!pdata) {
		dev_warn(&client->adapter->dev,
			 "Warning no platform data for mpu3050\n");
	} else {
		mldl_cfg->pdata = pdata;

		pdata->accel.get_slave_descr = get_accel_slave_descr;
		pdata->compass.get_slave_descr = get_compass_slave_descr;
		pdata->pressure.get_slave_descr = get_pressure_slave_descr;

		if (pdata->accel.get_slave_descr) {
			mldl_cfg->accel = pdata->accel.get_slave_descr();
			dev_info(&client->adapter->dev,
				 "%s: +%s\n", MPU_NAME, mldl_cfg->accel->name);
			accel_adapter = i2c_get_adapter(pdata->accel.adapt_num);
		} else {
			dev_warn(&client->adapter->dev,
				 "%s: No Accel Present\n", MPU_NAME);
		}

		if (pdata->compass.get_slave_descr) {
			mldl_cfg->compass = pdata->compass.get_slave_descr();
			dev_info(&client->adapter->dev,
				 "%s: +%s\n", MPU_NAME,
				 mldl_cfg->compass->name);
			compass_adapter =
			    i2c_get_adapter(pdata->compass.adapt_num);
		} else {
			dev_warn(&client->adapter->dev,
				 "%s: No Compass Present\n", MPU_NAME);
		}

		if (pdata->pressure.get_slave_descr) {
			mldl_cfg->pressure = pdata->pressure.get_slave_descr();
			dev_info(&client->adapter->dev,
				 "%s: +%s\n", MPU_NAME,
				 mldl_cfg->pressure->name);
			pressure_adapter =
			    i2c_get_adapter(pdata->pressure.adapt_num);

			if (pdata->pressure.irq > 0) {
				dev_info(&client->adapter->dev,
					 "Installing Pressure irq using %d\n",
					 pdata->pressure.irq);
				res = slaveirq_init(pressure_adapter,
						    &pdata->pressure,
						    "pressureirq");
				if (res)
					goto out_pressureirq_failed;
			} else {
				dev_warn(&client->adapter->dev,
					 "WARNING: Pressure irq not assigned\n");
			}
		} else {
			dev_warn(&client->adapter->dev,
				 "%s: No Pressure Present\n", MPU_NAME);
		}
	}

	mldl_cfg->addr = client->addr;
	res = mpu3050_open(&mpu->mldl_cfg, client->adapter,
			   accel_adapter, compass_adapter, pressure_adapter);

	if (res) {
		dev_err(&client->adapter->dev,
			"Unable to open %s %d\n", MPU_NAME, res);
		res = -ENODEV;
		goto out_whoami_failed;
	}

	res = misc_register(&i2c_mpu_device);
	if (res < 0) {
		dev_err(&client->adapter->dev,
			"ERROR: misc_register returned %d\n", res);
		goto out_misc_register_failed;
	}

	if (client->irq > 0) {
		dev_info(&client->adapter->dev,
			 "Installing irq using %d\n", client->irq);
		res = mpuirq_init(client);
		if (res)
			goto out_mpuirq_failed;
	} else {
		dev_warn(&client->adapter->dev,
			 "WARNING: %s irq not assigned\n", MPU_NAME);
	}

	mpu_accel_init(&mpu->mldl_cfg, client->adapter);

#ifdef CONFIG_HAS_EARLYSUSPEND
	mpu->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 1;
	mpu->early_suspend.suspend = mpu3050_early_suspend;
	mpu->early_suspend.resume = mpu3050_early_resume;
	register_early_suspend(&mpu->early_suspend);
#endif
	return res;

out_mpuirq_failed:
	misc_deregister(&i2c_mpu_device);
out_misc_register_failed:
	mpu3050_close(&mpu->mldl_cfg, client->adapter,
		      accel_adapter, compass_adapter, pressure_adapter);
out_whoami_failed:
	if (pdata && pdata->pressure.get_slave_descr && pdata->pressure.irq)
		slaveirq_exit(&pdata->pressure);
out_pressureirq_failed:
	kfree(mpu);
out_alloc_data_failed:
out_check_functionality_failed:
	dev_err(&client->adapter->dev, "%s failed %d\n", __func__, res);
	return res;

}

static int mpu3050_remove(struct i2c_client *client)
{
	struct mpu_private_data *mpu = i2c_get_clientdata(client);
	struct i2c_adapter *accel_adapter;
	struct i2c_adapter *compass_adapter;
	struct i2c_adapter *pressure_adapter;
	struct mldl_cfg *mldl_cfg = &mpu->mldl_cfg;
	struct mpu3050_platform_data *pdata = mldl_cfg->pdata;

	accel_adapter = i2c_get_adapter(mldl_cfg->pdata->accel.adapt_num);
	compass_adapter = i2c_get_adapter(mldl_cfg->pdata->compass.adapt_num);
	pressure_adapter = i2c_get_adapter(mldl_cfg->pdata->pressure.adapt_num);

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&mpu->early_suspend);
#endif
	mpu3050_close(mldl_cfg, client->adapter,
		      accel_adapter, compass_adapter, pressure_adapter);

	if (client->irq)
		mpuirq_exit();

	if (pdata && pdata->pressure.get_slave_descr && pdata->pressure.irq)
		slaveirq_exit(&pdata->pressure);

	if (pdata && pdata->compass.get_slave_descr && pdata->compass.irq)
		slaveirq_exit(&pdata->compass);

	if (pdata && pdata->accel.get_slave_descr && pdata->accel.irq)
		slaveirq_exit(&pdata->accel);

	misc_deregister(&i2c_mpu_device);
	kfree(mpu);

	mpu_accel_exit(mldl_cfg);

	return 0;
}

static const struct i2c_device_id mpu3050_id[] = {
	{MPU_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, mpu3050_id);

static struct i2c_driver mpu3050_driver = {
	.class = I2C_CLASS_HWMON,
	.probe = mpu3050_probe,
	.remove = mpu3050_remove,
	.id_table = mpu3050_id,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = MPU_NAME,
		   },
	.address_list = normal_i2c,
	.shutdown = mpu_shutdown,	/* optional */
	.suspend = mpu_suspend,	/* optional */
	.resume = mpu_resume,	/* optional */

};

static int __init mpu_init(void)
{
	int res = i2c_add_driver(&mpu3050_driver);
	pr_info("%s\n", __func__);
	if (res)
		pr_err("%s failed\n", __func__);
	return res;
}

static void __exit mpu_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&mpu3050_driver);
}

module_init(mpu_init);
module_exit(mpu_exit);

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("User space character device interface for MPU3050");
MODULE_LICENSE("GPL");
MODULE_ALIAS(MPU_NAME);
