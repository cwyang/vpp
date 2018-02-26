#!/usr/bin/env python
""" Vpp VCL tests """

import unittest
import os
import signal
from framework import VppTestCase, VppTestRunner, Worker
from vpp_ip_route import VppIpTable, VppIpRoute, VppRoutePath


class VclAppWorker(Worker):
    """ VCL Test Application Worker """

    def __init__(self, appname, args, logger, env={}):
        var = "VPP_TEST_BUILD_DIR"
        build_dir = os.getenv(var, None)
        if build_dir is None:
            raise Exception("Environment variable `%s' not set" % var)
        vcl_app_dir = "%s/vpp/.libs" % build_dir
        self.args = ["%s/%s" % (vcl_app_dir, appname)] + args
        super(VclAppWorker, self).__init__(self.args, logger, env)


class VclTestCase(VppTestCase):
    """ VCL Test Class """

    def validateResults(self, worker_client, worker_server, timeout):
        self.logger.info("Client worker result is `%s'" % worker_client.result)
        error = False
        if worker_client.result is None:
            try:
                error = True
                self.logger.error(
                    "Timeout! Client worker did not finish in %ss" % timeout)
                os.killpg(os.getpgid(worker_client.process.pid),
                          signal.SIGTERM)
                worker_client.join()
            except:
                self.logger.debug(
                    "Couldn't kill client worker-spawned process")
                raise
        if error:
            os.killpg(os.getpgid(worker_server.process.pid), signal.SIGTERM)
            worker_server.join()
            raise Exception(
                "Timeout! Client worker did not finish in %ss" % timeout)
        self.assert_equal(worker_client.result, 0, "Binary test return code")


class VCLCUTTHRUTestCase(VclTestCase):
    """ VPP Communications Library Test """

    server_addr = "127.0.0.1"
    server_port = "22000"
    timeout = 3
    echo_phrase = "Hello, world! Jenny is a friend of mine"

    def setUp(self):
        super(VCLCUTTHRUTestCase, self).setUp()

        self.vapi.session_enable_disable(is_enabled=1)

    def tearDown(self):
        self.vapi.session_enable_disable(is_enabled=0)

        super(VCLCUTTHRUTestCase, self).tearDown()

    def test_vcl_cutthru(self):
        """ run VCL cut-thru test """
        self.env = {'VCL_API_PREFIX': self.shm_prefix,
                    'VCL_APP_SCOPE_LOCAL': "true"}

        worker_server = VclAppWorker("vcl_test_server",
                                     [self.server_port],
                                     self.logger, self.env)
        worker_server.start()
        self.sleep(0.2)
        worker_client = VclAppWorker("vcl_test_client",
                                     [self.server_addr, self.server_port,
                                      "-E", self.echo_phrase, "-X"],
                                     self.logger, self.env)
        worker_client.start()
        worker_client.join(self.timeout)
        self.validateResults(worker_client, worker_server, self.timeout)


class VCLTHRUHSTestcase(VclTestCase):
    """ VCL Thru Hoststack Test """

    server_port = "22000"
    timeout = 3
    echo_phrase = "Hello, world! Jenny is a friend of mine"

    def setUp(self):
        super(VCLTHRUHSTestcase, self).setUp()

        self.vapi.session_enable_disable(is_enabled=1)
        self.create_loopback_interfaces(range(2))

        table_id = 0

        for i in self.lo_interfaces:
            i.admin_up()

            if table_id != 0:
                tbl = VppIpTable(self, table_id)
                tbl.add_vpp_config()

            i.set_table_ip4(table_id)
            i.config_ip4()
            table_id += 1

        # Configure namespaces
        self.vapi.app_namespace_add(namespace_id="0", secret=1234,
                                    sw_if_index=self.loop0.sw_if_index)
        self.vapi.app_namespace_add(namespace_id="1", secret=5678,
                                    sw_if_index=self.loop1.sw_if_index)

    def tearDown(self):
        for i in self.lo_interfaces:
            i.unconfig_ip4()
            i.set_table_ip4(0)
            i.admin_down()

        self.vapi.session_enable_disable(is_enabled=0)
        super(VCLTHRUHSTestcase, self).tearDown()

    def test_vcl_thru_hoststack(self):
        """ run VCL thru hoststack test """
        self.env = {'VCL_API_PREFIX': self.shm_prefix,
                    'VCL_APP_SCOPE_GLOBAL': "true"}

        # Add inter-table routes
        ip_t01 = VppIpRoute(self, self.loop1.local_ip4, 32,
                            [VppRoutePath("0.0.0.0",
                                          0xffffffff,
                                          nh_table_id=1)])
        ip_t10 = VppIpRoute(self, self.loop0.local_ip4, 32,
                            [VppRoutePath("0.0.0.0",
                                          0xffffffff,
                                          nh_table_id=0)], table_id=1)
        ip_t01.add_vpp_config()
        ip_t10.add_vpp_config()

        self.env.update({'VCL_APP_NAMESPACE_ID': "0",
                         'VCL_APP_NAMESPACE_SECRET': "1234"})
        worker_server = VclAppWorker("vcl_test_server",
                                     [self.server_port],
                                     self.logger, self.env)
        worker_server.start()
        self.sleep(0.2)

        self.env.update({'VCL_APP_NAMESPACE_ID': "1",
                         'VCL_APP_NAMESPACE_SECRET': "5678"})
        worker_client = VclAppWorker("vcl_test_client",
                                     [self.loop0.local_ip4, self.server_port,
                                      "-E", self.echo_phrase, "-X"],
                                     self.logger, self.env)
        worker_client.start()
        worker_client.join(self.timeout)

        self.validateResults(worker_client, worker_server, self.timeout)

if __name__ == '__main__':
    unittest.main(testRunner=VppTestRunner)
