from distributed import get_worker
from distributed.comm.addressing import parse_host_port
from dask.distributed import default_client
from ucp.endpoint_reuse import EndpointReuse
from distributed.comm.ucx import UCXListener
from distributed.comm.ucx import UCXConnector
import netifaces as ni
import random
import socket
import errno


def set_id_mappings_on_worker(mapping):
    worker = get_worker()
    worker.ucx_addresses = mapping


async def init_endpoints():
    for addr in get_worker().ucx_addresses.values():
        await UCX.get().get_endpoint(addr)


def checkSocket(socketNum):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM, 0)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    socket_free = False
    try:
        s.bind(("127.0.0.1", socketNum))
        socket_free = True
    except socket.error as e:
        if e.errno == errno.EADDRINUSE:
            socket_free = False
        else:
            # something else raised the socket.error exception
            print("ERROR: Something happened when checking socket " + str(socketNum))
    s.close()
    return socket_free


def get_communication_port(network_interface):
    ralCommunicationPort = random.randint(10000, 32000)
    workerIp = ni.ifaddresses(network_interface)[ni.AF_INET][0]["addr"]
    while checkSocket(ralCommunicationPort) is False:
        ralCommunicationPort = random.randint(10000, 32000)
    return {"port": ralCommunicationPort, "ip": workerIp}


def listen(client, network_interface=""):
    worker_id_maps = client.run(get_communication_port, network_interface, wait=True)
    print(worker_id_maps)
    client.run(set_id_mappings_on_worker, worker_id_maps, wait=True)
    return worker_id_maps


def cleanup(client=None):
    async def kill_ucx():
        await UCX.get().stop_endpoints()
        UCX.get().stop_listener()

    client = client if client is not None else default_client()
    return client.run(kill_ucx, wait=True)


class UCX:
    """
    Singleton UCX context to encapsulate all interactions with the
    UCX-py API and guarantee only a single listener & endpoints are
    created by cuML on a single process.
    """

    __instance = None

    def __init__(self):

        self.callback = None
        self._endpoints = {}
        self._listener = None
        self.received = 0
        self.sent = 0

        assert UCX.__instance is None

        UCX.__instance = self

    @staticmethod
    def get():
        if UCX.__instance is None:
            UCX()
        return UCX.__instance

    # @staticmethod
    # async def start_listener_on_worker(callback):
    #     UCX.get().callback = callback
    #     return await UCX.get().start_listener()

    # @staticmethod
    # async def init_handlers():
    #     addresses = get_worker().ucx_addresses
    #     eps = []
    #     for address in addresses.values():
    #         ep = await UCX.get().get_endpoint(address)

    # @staticmethod
    # def get_ucp_worker():
    #     return ucp.get_ucp_worker()

    # async def start_listener(self):

    #     ip, port = parse_host_port(get_worker().address)

    #     async def handle_comm(comm):
    #         print("oh fuck...")
    #         should_stop = False
    #         while not comm.closed() and not should_stop:
    #             msg = await comm.read()
    #             if msg == CTRL_STOP:
    #                 should_stop = True
    #             else:
    #                 msg = BlazingMessage(**{k: v.deserialize() for k, v in msg.items()})
    #                 self.received += 1
    #                 await self.callback(msg)

    #     self._listener = await UCXListener(ip, handle_comm)

    #     await self._listener.start()

    #     return "ucx://%s:%s" % (ip, self.listener_port())

    def listener_port(self):
        return self._listener.port

    async def _create_endpoint(self, addr):
        ep = await UCXConnector().connect(addr)
        self._endpoints[addr] = ep
        return ep

    async def get_endpoint(self, addr):
        if addr not in self._endpoints:
            ep = await self._create_endpoint(addr)
        else:
            ep = self._endpoints[addr]

        return ep

    def abort_endpoints(self):
        for addr, ep in self._endpoints.items():
            if not ep.closed():
                ep.abort()
            del ep
        self._endpoints = {}

    async def stop_endpoints(self):
        for addr, ep in self._endpoints.items():
            if not ep.closed():
                await ep.close()
            del ep
        self._endpoints = {}

    def stop_listener(self):
        if self._listener is not None:
            self._listener.stop()

    def __del__(self):
        self.abort_endpoints()
        self.stop_listener()