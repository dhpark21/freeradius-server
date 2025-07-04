-----------------------------------------------------------------------------
-- $Id$                 	   --
--                                                                         --
--  schema.sql                       rlm_sql - FreeRADIUS SQLite Module    --
--                                                                         --
--     Database schema for SQLite rlm_sql module                           --
--                                                                         --
-----------------------------------------------------------------------------

--
-- To aid performance in a multi-threaded system so that reads are not
-- blocked by writes, consider using the write-ahead log
-- https://www.sqlite.org/wal.html
--
-- PRAGMA journal_mode=WAL;

--
-- Table structure for table 'radacct'
--
CREATE TABLE radacct (
	radacctid INTEGER PRIMARY KEY AUTOINCREMENT,
	acctsessionid varchar(64) NOT NULL default '',
	acctuniqueid varchar(32) NOT NULL default '',
	username varchar(64) NOT NULL default '',
	groupname varchar(64) NOT NULL default '',
	realm varchar(64) default '',
	nasipaddress varchar(15) NOT NULL default '',
	nasportid varchar(32) default NULL,
	nasporttype varchar(32) default NULL,
	acctstarttime datetime NULL default NULL,
	acctupdatetime datetime NULL default NULL,
	acctstoptime datetime NULL default NULL,
	acctinterval int(12) default NULL,
	acctsessiontime int(12) default NULL,
	acctauthentic varchar(32) default NULL,
	connectinfo_start varchar(50) default NULL,
	connectinfo_stop varchar(50) default NULL,
	acctinputoctets bigint(20) default NULL,
	acctoutputoctets bigint(20) default NULL,
	calledstationid varchar(50) NOT NULL default '',
	callingstationid varchar(50) NOT NULL default '',
	acctterminatecause varchar(32) NOT NULL default '',
	servicetype varchar(32) default NULL,
	framedprotocol varchar(32) default NULL,
	framedipaddress varchar(15) NOT NULL default '',
	framedipv6address varchar(45) NOT NULL default '',
	framedipv6prefix varchar(45) NOT NULL default '',
	framedinterfaceid varchar(44) NOT NULL default '',
	delegatedipv6prefix varchar(45) NOT NULL default '',
	class varchar(64) default NULL
);

CREATE UNIQUE INDEX acctuniqueid ON radacct(acctuniqueid);
CREATE INDEX username ON radacct(username);
CREATE INDEX framedipaddress ON radacct (framedipaddress);
CREATE INDEX framedipv6address ON radacct (framedipv6address);
CREATE INDEX framedipv6prefix ON radacct (framedipv6prefix);
CREATE INDEX framedinterfaceid ON radacct (framedinterfaceid);
CREATE INDEX delegatedipv6prefix ON radacct (delegatedipv6prefix);
CREATE INDEX acctsessionid ON radacct(acctsessionid);
CREATE INDEX acctsessiontime ON radacct(acctsessiontime);
CREATE INDEX acctstarttime ON radacct(acctstarttime);
CREATE INDEX acctinterval ON radacct(acctinterval);
CREATE INDEX acctstoptime ON radacct(acctstoptime);
CREATE INDEX nasipaddress ON radacct(nasipaddress);

-- For use by onoff query
CREATE INDEX radacct_bulk_close ON radacct(acctstoptime, nasipaddress, acctstarttime);

--
-- Table structure for table 'radcheck'
--
CREATE TABLE radcheck (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	username varchar(64) NOT NULL default '',
	attribute varchar(64)  NOT NULL default '',
	op char(2) NOT NULL DEFAULT '==',
	value varchar(253) NOT NULL default ''
);
CREATE INDEX check_username ON radcheck(username);

--
-- Table structure for table 'radgroupcheck'
--
CREATE TABLE radgroupcheck (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	groupname varchar(64) NOT NULL default '',
	attribute varchar(64)  NOT NULL default '',
	op char(2) NOT NULL DEFAULT '==',
	value varchar(253)  NOT NULL default ''
);
CREATE INDEX check_groupname ON radgroupcheck(groupname);

--
-- Table structure for table 'radgroupreply'
--
CREATE TABLE radgroupreply (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	groupname varchar(64) NOT NULL default '',
	attribute varchar(64)  NOT NULL default '',
	op char(2) NOT NULL DEFAULT '=',
	value varchar(253)  NOT NULL default ''
);
CREATE INDEX reply_groupname ON radgroupreply(groupname);

--
-- Table structure for table 'radreply'
--
CREATE TABLE radreply (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	username varchar(64) NOT NULL default '',
	attribute varchar(64) NOT NULL default '',
	op char(2) NOT NULL DEFAULT '=',
	value varchar(253) NOT NULL default ''
);
CREATE INDEX reply_username ON radreply(username);

--
-- Table structure for table 'radusergroup'
--
CREATE TABLE radusergroup (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	username varchar(64) NOT NULL default '',
	groupname varchar(64) NOT NULL default '',
	priority int(11) NOT NULL default '1'
);
CREATE INDEX usergroup_username ON radusergroup(username);

--
-- Table structure for table 'radpostauth'
--
CREATE TABLE radpostauth (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	username varchar(64) NOT NULL default '',
	pass varchar(64) NOT NULL default '',
	reply varchar(32) NOT NULL default '',
	authdate timestamp NOT NULL,
	class varchar(64) NOT NULL default ''
);

--
-- Table structure for table 'nas'
--
CREATE TABLE nas (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	nasname varchar(128) NOT NULL,
	shortname varchar(32),
	type varchar(30) DEFAULT 'other',
	ports int(5),
	secret varchar(60) DEFAULT 'secret' NOT NULL,
	server varchar(64),
	community varchar(50),
	description varchar(200) DEFAULT 'RADIUS Client',
	require_ma varchar(4) DEFAULT 'auto',
	limit_proxy_state varchar(4) DEFAULT 'auto'
);
CREATE INDEX nasname ON nas(nasname);

--
-- Table structure for table 'nasreload'
--
CREATE TABLE IF NOT EXISTS nasreload (
	nasipaddress varchar(15) PRIMARY KEY,
	reloadtime datetime NOT NULL
);
