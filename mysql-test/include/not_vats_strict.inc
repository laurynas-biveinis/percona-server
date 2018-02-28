if (`SELECT @@innodb_lock_schedule_algorithm = 'VATS_STRICT'`)
{
  skip Test cannot be run with innodb_lock_schedule_algorithm set to VATS_STRICT;
}
