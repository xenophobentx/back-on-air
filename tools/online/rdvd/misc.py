"""The polled and optional services, answered so that the menus work: Friends (20) for now.

Friends method 10 is asked by the Online menu about four times a second until it gets an answer (parameters: a
relationship filter byte and a flag). Nobody has friends yet: an empty list.
"""
from .codec import Writer
from .server import rmc


@rmc("secure", 20, 10)
def friends_list(server, c, r):
    r.u8()
    r.bool()
    return Writer().u32(0).bytes()
