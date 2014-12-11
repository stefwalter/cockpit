

int
main(int argc,
     char *argv)
{
  GUdevClient *client = NULL;
  GUdevDevice *device = NULL;
  GString *str;
  gchar *s;

  /* TODO: Probably need some other and/or better heuristics to yield
   * useful info on as many systems as possible
   */

  client = g_udev_client_new (NULL);
  device = g_udev_client_query_by_sysfs_path (client, "/sys/devices/virtual/dmi/id");
  if (device == NULL)
    goto out;

  str = g_string_new (NULL);
  g_string_append_printf (str, "%s %s (%s)",
                          g_udev_device_get_sysfs_attr (device, "bios_vendor"),
                          g_udev_device_get_sysfs_attr (device, "bios_version"),
                          g_udev_device_get_sysfs_attr (device, "bios_date"));
  cockpit_manager_set_bios (COCKPIT_MANAGER (manager), str->str);
  g_string_free (str, TRUE);

  str = g_string_new (NULL);
  g_string_append_printf (str, "%s %s",
                          g_udev_device_get_sysfs_attr (device, "sys_vendor"),
                          g_udev_device_get_sysfs_attr (device, "product_name"));
  s = get_stripped_sysfs_attr (device, "product_version");
  if (s != NULL)
    {
      g_string_append_printf (str, " (%s)", s);
      g_free (s);
    }
  cockpit_manager_set_system (COCKPIT_MANAGER (manager), str->str);
  g_string_free (str, TRUE);

  s = get_stripped_sysfs_attr (device, "product_serial");
  if (s == NULL)
    s = get_stripped_sysfs_attr (device, "chassis_serial");
  cockpit_manager_set_system_serial (COCKPIT_MANAGER (manager), s);

out:
  g_clear_object (&device);
  g_clear_object (&client);
}

  
